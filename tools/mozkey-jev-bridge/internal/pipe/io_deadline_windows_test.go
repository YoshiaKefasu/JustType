//go:build windows

package pipe

import (
	"os"
	"runtime"
	"testing"
	"time"

	"golang.org/x/sys/windows"
)

// createConnectedPipe creates one pipe instance and returns it together with a
// connected client file, so a test can drive the server side directly.
func createConnectedPipe(t *testing.T, name string) (windows.Handle, *os.File) {
	t.Helper()

	// Pinned because arm() cancels by OS thread and the goroutine must not
	// migrate between arming and the blocking call under test.
	runtime.LockOSThread()
	t.Cleanup(runtime.UnlockOSThread)

	attributes, err := currentUserOnlySecurityAttributes()
	if err != nil {
		t.Fatalf("currentUserOnlySecurityAttributes: %v", err)
	}

	wideName, err := windows.UTF16PtrFromString(name)
	if err != nil {
		t.Fatalf("UTF16PtrFromString: %v", err)
	}

	instance, err := windows.CreateNamedPipe(
		wideName,
		windows.PIPE_ACCESS_DUPLEX,
		windows.PIPE_TYPE_BYTE|windows.PIPE_READMODE_BYTE|windows.PIPE_WAIT,
		windows.PIPE_UNLIMITED_INSTANCES,
		65536,
		65536,
		0,
		attributes,
	)
	if err != nil {
		t.Fatalf("CreateNamedPipe: %v", err)
	}

	connected := make(chan struct{})
	go func() {
		close(connected)
		// The client only opens the pipe; it never writes, which is exactly the
		// stall the watchdog exists to break.
		client, err := os.OpenFile(name, os.O_RDWR, 0)
		if err == nil {
			t.Cleanup(func() { client.Close() })
		}
		windows.ConnectNamedPipe(instance, nil)
	}()

	<-connected
	if err := windows.ConnectNamedPipe(instance, nil); err != nil &&
		!isErrno(err, errnoPipeConnected) {
		t.Fatalf("ConnectNamedPipe: %v", err)
	}

	t.Cleanup(func() { windows.CloseHandle(instance) })
	return instance, nil
}

// A client that connects and then writes nothing must not wedge the accept loop
// forever.  Without the watchdog this read blocks indefinitely.
func TestIODeadlineUnblocksStalledRead(t *testing.T) {
	instance, _ := createConnectedPipe(t, testPipeNameInternal("stalled_read"))

	deadline := arm(instance, 200*time.Millisecond)
	if !deadline.valid() {
		t.Fatal("watchdog was not armed; the test cannot prove anything")
	}

	conn := os.NewFile(uintptr(instance), "stalled_read")

	done := make(chan error, 1)
	go func() {
		buffer := make([]byte, 32)
		_, err := conn.Read(buffer)
		done <- err
	}()

	select {
	case err := <-done:
		if err == nil {
			t.Error("read unexpectedly succeeded; the client never wrote")
		}
	case <-time.After(10 * time.Second):
		t.Fatal("read was not cancelled; the accept loop would wedge here")
	}

	// release must join the watchdog, so a late cancellation cannot land on a
	// subsequent connection.
	deadline.release()
	conn.Close()
}

// release must join the watchdog.  With an hour-long timeout, the only way
// release returns promptly is by actually stopping the goroutine; otherwise
// wg.Wait would block for the full hour.
func TestIODeadlineReleaseStopsTheWatchdog(t *testing.T) {
	instance, _ := createConnectedPipe(t, testPipeNameInternal("release"))

	deadline := arm(instance, time.Hour)
	if !deadline.valid() {
		t.Fatal("watchdog was not armed")
	}

	start := time.Now()
	deadline.release()
	if elapsed := time.Since(start); elapsed > 2*time.Second {
		t.Errorf("release took %v; the watchdog did not stop", elapsed)
	}

	// The accept loop calls release unconditionally, so a second call must be a
	// no-op rather than a double close.
	deadline.release()
}

func TestIODeadlineWithoutThreadHandleIsInvalid(t *testing.T) {
	deadline := &ioDeadline{done: make(chan struct{})}
	if deadline.valid() {
		t.Error("a deadline with no thread handle must report invalid")
	}
	// release must be safe to call on a never-armed deadline.
	deadline.release()
}

func TestSafetyTimeoutsMatchReference(t *testing.T) {
	// src/zenz_scorer/main.cc:
	//   kPipeIoSafetyTimeoutMsec = kMinLlamaReadyWaitMsec(1500) +
	//                              kMaxRequestTimeoutMsec(5000) + 1500 = 8000
	//   kPipeFlushSafetyTimeoutMsec = 1000
	if IoSafetyTimeout != 8000*time.Millisecond {
		t.Errorf("IoSafetyTimeout = %v, want 8s", IoSafetyTimeout)
	}
	if FlushSafetyTimeout != 1000*time.Millisecond {
		t.Errorf("FlushSafetyTimeout = %v, want 1s", FlushSafetyTimeout)
	}
	if connectFailureBackoff != 100*time.Millisecond {
		t.Errorf("connectFailureBackoff = %v, want 100ms", connectFailureBackoff)
	}
}

func testPipeNameInternal(tag string) string {
	return `\\.\pipe\mozkey_jev_bridge_io_deadline_` + tag + "_" +
		time.Now().Format("150405.000000000")
}
