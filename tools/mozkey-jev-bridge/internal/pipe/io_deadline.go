//go:build windows

package pipe

import (
	"errors"
	"log"
	"sync"
	"time"

	"golang.org/x/sys/windows"
)

// Safety timeouts, mirroring src/zenz_scorer/main.cc.
//
//	kPipeIoSafetyTimeoutMsec   = kMinLlamaReadyWaitMsec + kMaxRequestTimeoutMsec + 1500 = 8000
//	kPipeFlushSafetyTimeoutMsec = 1000
//
// The I/O budget is far larger than the model budget because it also has to
// cover reading the frame and writing the response, not just the inference.
const (
	IoSafetyTimeout    = 8000 * time.Millisecond
	FlushSafetyTimeout = 1000 * time.Millisecond
)

// cancelInterval is how often the watchdog re-issues cancellation after the
// deadline expires.  The reference implementation repeats because the issuing
// thread may be between two I/O calls when the first cancellation lands.
const cancelInterval = 10 * time.Millisecond

// cancelSynchronousIo cancels synchronous I/O issued by a thread.
//
// golang.org/x/sys/windows does not export it, so the entry point is resolved
// directly.  This is the only API that unblocks a synchronous ReadFile on a
// non-overlapped pipe handle, which is why the bridge needs it alongside
// CancelIoEx.
var cancelSynchronousIo = windows.NewLazySystemDLL("kernel32.dll").
	NewProc("CancelSynchronousIo")

// ioDeadline bounds a connected pipe's synchronous I/O, including
// FlushFileBuffers.
//
// It is a port of mozc::zenz::SynchronousIoDeadline from
// src/zenz_scorer/synchronous_io_deadline.h.  The issuing goroutine retains
// ownership of every I/O buffer and of the pipe handle; release must be called
// before leaving the connection so a cancellation cannot land on a subsequent
// connection or a recycled handle.
type ioDeadline struct {
	// handle is the pipe handle, or 0 to cancel only by thread.
	handle windows.Handle
	// thread is a handle to the issuing thread opened with THREAD_TERMINATE.
	thread windows.Handle

	done chan struct{}
	wg   sync.WaitGroup
	once sync.Once
}

// arm starts a watchdog for the calling goroutine's OS thread.
//
// The caller must have pinned the goroutine with runtime.LockOSThread before
// calling arm, otherwise the Go runtime may migrate the goroutine to another OS
// thread and CancelSynchronousIo would target the wrong one.
func arm(handle windows.Handle, timeout time.Duration) *ioDeadline {
	deadline := &ioDeadline{
		handle: handle,
		done:   make(chan struct{}),
	}

	// CancelSynchronousIo requires THREAD_TERMINATE on the target handle.
	// Open it explicitly instead of relying on the pseudo-handle's inherited
	// access mask, which can be insufficient under a restricted token.
	thread, err := windows.OpenThread(windows.THREAD_TERMINATE, false,
		windows.GetCurrentThreadId())
	if err != nil {
		// The reference implementation also proceeds unguarded here.  Log it
		// loudly, because an unwatchdogged connection can block forever.
		log.Printf("pipe: OpenThread(THREAD_TERMINATE) failed, connection is unwatchdogged: %v", err)
		return deadline
	}
	deadline.thread = thread

	deadline.wg.Add(1)
	go deadline.watch(timeout)
	return deadline
}

func (d *ioDeadline) watch(timeout time.Duration) {
	defer d.wg.Done()

	timer := time.NewTimer(timeout)
	defer timer.Stop()

	select {
	case <-d.done:
		return
	case <-timer.C:
	}

	ticker := time.NewTicker(cancelInterval)
	defer ticker.Stop()

	for {
		d.cancel()

		select {
		case <-d.done:
			return
		case <-ticker.C:
		}
	}
}

// cancel asks Windows to abort whatever the issuing thread is blocked in.
func (d *ioDeadline) cancel() {
	if d.handle != 0 {
		if err := windows.CancelIoEx(d.handle, nil); err != nil {
			// ERROR_NOT_FOUND means no I/O was pending, which is harmless when
			// the issuing thread is between two calls.
			if !isErrno(err, windows.ERROR_NOT_FOUND) {
				log.Printf("pipe: CancelIoEx: %v", err)
			}
		}
	}
	if d.thread == 0 {
		return
	}

	//nolint:errcheck // the return is inspected below
	r1, _, callErr := cancelSynchronousIo.Call(uintptr(d.thread))
	if r1 == 0 && !isErrno(callErr, windows.ERROR_NOT_FOUND) {
		// ERROR_NOT_FOUND means the thread had no synchronous I/O pending, which
		// is expected when the issuing thread is between two calls.
		log.Printf("pipe: CancelSynchronousIo: %v", callErr)
	}
}

// release stops the watchdog and waits for it to finish, so no cancellation can
// outlive the connection.
func (d *ioDeadline) release() {
	if d.thread == 0 {
		return
	}
	d.once.Do(func() {
		close(d.done)
		d.wg.Wait()
		windows.CloseHandle(d.thread)
	})
}

// valid reports whether the watchdog was actually armed.
func (d *ioDeadline) valid() bool {
	return d != nil && d.thread != 0
}

// isErrno reports whether err is a specific Windows errno.
func isErrno(err error, target windows.Errno) bool {
	if err == nil {
		return false
	}
	var errno windows.Errno
	if !errors.As(err, &errno) {
		return false
	}
	return errno == target
}
