//go:build windows

package pipe

import (
	"errors"
	"strings"
	"testing"
	"unsafe"

	"golang.org/x/sys/windows"
)

// The pipe forwards typed Japanese text to a remote model, so the DACL must
// grant only the current user and SYSTEM.  Everyone or a low integrity label
// would expose every keystroke to other processes on the machine.
func TestCurrentUserOnlySDDL(t *testing.T) {
	sddl, err := currentUserOnlySDDL()
	if err != nil {
		t.Fatalf("currentUserOnlySDDL: %v", err)
	}

	if !strings.HasPrefix(sddl, "D:P") {
		t.Errorf("SDDL = %q, want a protected DACL", sddl)
	}
	if !strings.Contains(sddl, "A;;GRGW;;;") {
		t.Errorf("SDDL = %q, want the current user to get generic read + write", sddl)
	}
	if !strings.Contains(sddl, "(A;;GA;;;SY)") {
		t.Errorf("SDDL = %q, want SYSTEM to get generic all", sddl)
	}

	// The account name of the current user's own SID must be present.
	sid, err := currentUserSID()
	if err != nil {
		t.Fatalf("currentUserSID: %v", err)
	}
	if !strings.HasPrefix(sid, "S-1-") {
		t.Errorf("SID = %q, want an S-1- form", sid)
	}
	if !strings.Contains(sddl, sid) {
		t.Errorf("SDDL = %q, want it to name SID %q", sddl, sid)
	}

	// WD is Everyone; an S:(...) section would be a mandatory integrity
	// label.  Either one would widen the pipe beyond this account.
	if strings.Contains(sddl, "WD") || strings.Contains(sddl, "Everyone") {
		t.Errorf("SDDL = %q, must not grant Everyone", sddl)
	}
	if strings.Contains(sddl, "S:(") {
		t.Errorf("SDDL = %q, must not carry a mandatory integrity label", sddl)
	}
}

func TestCurrentUserOnlySecurityAttributes(t *testing.T) {
	attributes, err := currentUserOnlySecurityAttributes()
	if err != nil {
		t.Fatalf("currentUserOnlySecurityAttributes: %v", err)
	}

	if attributes == nil {
		t.Fatal("attributes must not be nil")
	}
	if attributes.SecurityDescriptor == nil {
		t.Fatal("security descriptor must not be nil")
	}
	if attributes.InheritHandle != 0 {
		t.Error("the pipe handle must not be inheritable")
	}
	wantLength := uint32(unsafe.Sizeof(windows.SecurityAttributes{}))
	if attributes.Length != wantLength {
		t.Errorf("Length = %d, want %d", attributes.Length, wantLength)
	}
}

func TestSecurityDescriptorFromSDDLIsAccepted(t *testing.T) {
	sddl, err := currentUserOnlySDDL()
	if err != nil {
		t.Fatalf("currentUserOnlySDDL: %v", err)
	}

	// A malformed SDL is the failure mode this guards against: Windows rejects
	// the descriptor and the pipe would silently fall back to no DACL at all.
	descriptor, err := windows.SecurityDescriptorFromString(sddl)
	if err != nil {
		t.Fatalf("SecurityDescriptorFromString(%q): %v", sddl, err)
	}
	if descriptor == nil {
		t.Fatal("descriptor must not be nil")
	}

	if _, err := windows.SecurityDescriptorFromString("D:P(A;;GRGW;;;not-a-sid)"); err == nil {
		t.Error("expected an error for a malformed SDDL")
	}
}

// A second bridge on the same pipe name would silently split requests between
// two backends, so the guard must refuse rather than bind.
func TestAcquireSingleInstanceRefusesSecondHolder(t *testing.T) {
	name := `Local\MozkeyJevBridgeTestMutex`

	first, err := AcquireSingleInstance(name)
	if err != nil {
		t.Fatalf("first AcquireSingleInstance: %v", err)
	}
	if first == 0 {
		t.Fatal("first acquisition returned no handle")
	}
	defer windows.CloseHandle(first)

	if _, err := AcquireSingleInstance(name); !errors.Is(err, ErrAlreadyRunning) {
		t.Errorf("second acquisition error = %v, want ErrAlreadyRunning", err)
	}
}

func TestAcquireSingleInstanceDefaultsName(t *testing.T) {
	if DefaultSingleInstanceMutexName == "" {
		t.Fatal("DefaultSingleInstanceMutexName must not be empty")
	}
	if !strings.HasPrefix(DefaultSingleInstanceMutexName, `Local\`) {
		t.Errorf("mutex name = %q, want it under the Local namespace", DefaultSingleInstanceMutexName)
	}
	if DefaultSingleInstanceMutexName == `Local\MozcZenzScorerSingleInstance` {
		t.Error("the bridge must not share mozc_zenz_scorer's mutex name")
	}
}
