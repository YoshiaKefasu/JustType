//go:build windows

// Package pipe serves the ZNZ1 protocol over a Windows named pipe.
//
// The accept loop mirrors src/zenz_scorer/main.cc: one pipe instance per
// iteration, connect, read, process, write, flush, disconnect, repeat.  Handling
// one connection at a time is deliberate; Mozkey sends a single request and
// waits for the answer, and a burst beyond that queues on further instances
// where the client's own 3.5 s failsafe turns it into a normal conversion
// fallback instead of a pile-up of concurrent model calls.
//
// Data I/O uses blocking ReadFile/WriteFile through os.File, so the pipe is
// created without FILE_FLAG_OVERLAPPED and ConnectNamedPipe blocks.  A client
// that vanishes mid-request breaks the pipe and unblocks the read, so the loop
// cannot wedge on a dead peer.
package pipe

import (
	"errors"
	"fmt"
	"io"
	"log"
	"os"
	"runtime"
	"syscall"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"

	"mozkey-jev-bridge/internal/znz"
)

// DefaultPipeName is where Mozkey expects the local scorer.
const DefaultPipeName = `\\.\pipe\mozc_zenz_scorer`

// DefaultSingleInstanceMutexName guards against two bridges sharing one pipe
// name.  It mirrors mozc_zenz_scorer's Local\MozcZenzScorerSingleInstance but
// is distinct from it, since the two are separate programs.
const DefaultSingleInstanceMutexName = `Local\MozkeyJevBridgeSingleInstance`

// errnoAlreadyExists is ERROR_ALREADY_EXISTS, which x/sys does not export.
const errnoAlreadyExists = windows.Errno(183)

const defaultBufferSize = 65536

// errnoPipeConnected is ERROR_PIPE_CONNECTED: the client had already connected
// by the time ConnectNamedPipe ran, which is success rather than failure.
const errnoPipeConnected = syscall.Errno(0x217)

// Config configures the server.
type Config struct {
	// PipeName is the full pipe path, for example \\.\pipe\mozc_zenz_scorer.
	PipeName string
	// BufferSize is the inbound and outbound buffer size per instance.
	BufferSize int
}

// Outcome is what a HandleFunc produced for one request.
type Outcome struct {
	Value  string
	Status uint32
	Debug  string
}

// HandleFunc resolves one request.  It must not block for long and must never
// panic on malformed input.
type HandleFunc func(header znz.RequestHeader, prompt []byte) Outcome

// ErrAlreadyRunning reports that another bridge instance already holds the
// single-instance mutex.
var ErrAlreadyRunning = errors.New("pipe: another instance is already running")

// AcquireSingleInstance holds a named mutex for the lifetime of the process.
//
// Two bridges on the same pipe name do not fail loudly: Windows happily creates
// a second set of pipe instances under the same name, so requests would be
// split between two backends with no error anywhere.  mozc_zenz_scorer guards
// against this with Local\MozcZenzScorerSingleInstance, and the bridge needs
// the same guard.
//
// The returned handle must be kept alive for as long as the process serves.
func AcquireSingleInstance(name string) (windows.Handle, error) {
	if name == "" {
		name = DefaultSingleInstanceMutexName
	}

	wideName, err := windows.UTF16PtrFromString(name)
	if err != nil {
		return 0, fmt.Errorf("encode mutex name %q: %w", name, err)
	}

	// initialOwner=true takes ownership immediately.  Windows still returns a
	// valid handle when the name is taken and signals it with
	// ERROR_ALREADY_EXISTS, which is the case we treat as a refusal.
	handle, err := windows.CreateMutex(nil, true, wideName)
	if errors.Is(err, errnoAlreadyExists) {
		if handle != 0 {
			windows.CloseHandle(handle)
		}
		return 0, ErrAlreadyRunning
	}
	if err != nil {
		return 0, fmt.Errorf("create mutex %q: %w", name, err)
	}
	if handle == 0 {
		return 0, fmt.Errorf("create mutex %q: returned no handle", name)
	}
	return handle, nil
}

// Serve accepts connections until the process exits.
//
// It returns only when the security attributes cannot be built, since without
// them the pipe would be world-accessible and must not be created.
//
// The whole loop is pinned to one OS thread.  arm() cancels synchronous I/O by
// thread, so the goroutine must not migrate between arming the watchdog and the
// blocking call it guards.
func Serve(cfg Config, handle HandleFunc) error {
	// Pinned for the lifetime of the server; released by process exit.
	runtime.LockOSThread()

	securityAttributes, err := currentUserOnlySecurityAttributes()
	if err != nil {
		return fmt.Errorf("pipe: build security attributes: %w", err)
	}

	if cfg.PipeName == "" {
		cfg.PipeName = DefaultPipeName
	}
	if cfg.BufferSize <= 0 {
		cfg.BufferSize = defaultBufferSize
	}

	for {
		serveOnce(cfg, securityAttributes, handle)
	}
}

// serveOnce runs one create/connect/handle cycle.  It never returns an error
// that should stop the server; failures are logged and the loop retries.
func serveOnce(cfg Config, securityAttributes *windows.SecurityAttributes,
	handle HandleFunc) {
	name, err := windows.UTF16PtrFromString(cfg.PipeName)
	if err != nil {
		log.Printf("pipe: encode pipe name %q: %v", cfg.PipeName, err)
		time.Sleep(time.Second)
		return
	}

	instance, err := windows.CreateNamedPipe(
		name,
		windows.PIPE_ACCESS_DUPLEX,
		windows.PIPE_TYPE_BYTE|windows.PIPE_READMODE_BYTE|windows.PIPE_WAIT,
		windows.PIPE_UNLIMITED_INSTANCES,
		uint32(cfg.BufferSize),
		uint32(cfg.BufferSize),
		0,
		securityAttributes,
	)
	if err != nil {
		// A name collision or resource exhaustion will not fix itself in 10 ms,
		// so back off hard here.
		log.Printf("pipe: CreateNamedPipe %q: %v", cfg.PipeName, err)
		time.Sleep(time.Second)
		return
	}

	if err := windows.ConnectNamedPipe(instance, nil); err != nil &&
		!errors.Is(err, errnoPipeConnected) {
		windows.CloseHandle(instance)
		// A client that connects and immediately vanishes lands here, so this is
		// not fatal.  Back off anyway: a persistent accept failure would
		// otherwise spin as fast as the loop can create instances.
		log.Printf("pipe: ConnectNamedPipe: %v", err)
		time.Sleep(connectFailureBackoff)
		return
	}

	serveConnection(instance, cfg, handle)
}

// serveConnection owns one connection end to end and the whole lifetime of
// its handle: read, resolve, write, flush, disconnect, close.
//
// Every watchdog is joined before the handle is closed.  Closing first would
// let a pending CancelIoEx land on a handle value that CreateNamedPipe has
// already handed to the next connection.
func serveConnection(instance windows.Handle, cfg Config,
	handle HandleFunc) {
	conn := os.NewFile(uintptr(instance), cfg.PipeName)
	defer conn.Close()

	started := time.Now()

	// Bound the read/process/write cycle.  A client that connects and then writes
	// nothing would otherwise block here forever and wedge the single accept
	// loop.
	ioWatch := arm(instance, IoSafetyTimeout)

	request, err := znz.ReadRequest(conn)
	if err != nil {
		// A protocol violation means the client is alive and talking, so answer it
		// the way HandleClient does and only then disconnect.
		var violation *znz.ProtocolError
		if errors.As(err, &violation) {
			log.Printf("pipe: protocol violation: %v", err)
			if writeErr := znz.WriteResponse(conn, violation.Generation,
				znz.StatusError, 0, "", violation.Token()); writeErr != nil {
				log.Printf("pipe: write error response: %v", writeErr)
			}
			ioWatch.release()
			return
		}

		// A transport error means the client is gone mid-frame; there is no
		// request to answer and nobody left to read the answer.
		if !errors.Is(err, io.EOF) && !errors.Is(err, io.ErrUnexpectedEOF) {
			log.Printf("pipe: read request: %v", err)
		}
		ioWatch.release()
		return
	}

	outcome := handle(request.Header, request.Prompt)
	latency := uint32(time.Since(started).Milliseconds())

	if err := znz.WriteResponse(conn, request.Header.Generation, outcome.Status, latency,
		outcome.Value, outcome.Debug); err != nil {
		log.Printf("pipe: write response: %v", err)
	}

	// Join the watchdog before touching the handle any further.
	ioWatch.release()

	// FlushFileBuffers is the last synchronous call and has no overlapped form, so
	// it needs its own shorter watchdog.
	flushWatch := arm(instance, FlushSafetyTimeout)
	if err := windows.FlushFileBuffers(instance); err != nil {
		log.Printf("pipe: FlushFileBuffers: %v", err)
	}
	flushWatch.release()

	// Nothing is armed now, so the deferred conn.Close() cannot race a cancel.
	if err := windows.DisconnectNamedPipe(instance); err != nil {
		log.Printf("pipe: DisconnectNamedPipe: %v", err)
	}
}

// connectFailureBackoff keeps a persistent accept failure from spinning as
// fast as the loop can create pipe instances.
const connectFailureBackoff = 100 * time.Millisecond

// currentUserOnlySecurityAttributes builds the DACL the pipe is created with.
//
// It deliberately returns no cleanup: windows.SecurityDescriptorFromString
// documents that it copies the Windows-allocated descriptor onto the Go heap,
// so freeing it with LocalFree would release GC-managed memory and corrupt the
// heap.  The descriptor stays alive because the returned SECURITY_ATTRIBUTES
// points at it.
func currentUserOnlySecurityAttributes() (*windows.SecurityAttributes, error) {
	sddl, err := currentUserOnlySDDL()
	if err != nil {
		return nil, err
	}

	descriptor, err := windows.SecurityDescriptorFromString(sddl)
	if err != nil {
		return nil, fmt.Errorf("build descriptor from SDDL %q: %w", sddl, err)
	}

	return &windows.SecurityAttributes{
		Length:             uint32(unsafe.Sizeof(windows.SecurityAttributes{})),
		SecurityDescriptor: descriptor,
		InheritHandle:      0,
	}, nil
}

// currentUserOnlySDDL is the DACL the pipe is created with.
//
//	D:P              protected DACL: no inheritance from the parent
//	A;;GRGW;;;<SID>  current user: generic read + generic write
//	A;;GA;;;SY       SYSTEM: generic all
//
// WD (Everyone) and an S:(ML;;NW;;;LW) low integrity label are deliberately
// absent, matching src/zenz_scorer/main.cc.  The bridge forwards typed Japanese
// text to a remote model, so the pipe must not be reachable from another
// account or another integrity level.
func currentUserOnlySDDL() (string, error) {
	sid, err := currentUserSID()
	if err != nil {
		return "", fmt.Errorf("resolve current user SID: %w", err)
	}
	return "D:P(A;;GRGW;;;" + sid + ")(A;;GA;;;SY)", nil
}

func currentUserSID() (string, error) {
	var token windows.Token
	process := windows.CurrentProcess()
	if err := windows.OpenProcessToken(process, windows.TOKEN_QUERY, &token); err != nil {
		return "", fmt.Errorf("OpenProcessToken: %w", err)
	}
	defer token.Close()

	user, err := token.GetTokenUser()
	if err != nil {
		return "", fmt.Errorf("GetTokenUser: %w", err)
	}

	sid := user.User.Sid.String()
	if sid == "" {
		return "", errors.New("SID did not convert to string form")
	}
	return sid, nil
}
