package hub

import "fmt"

// Code is an error code in the API spec format (ErrorCode).
type Code string

// Error codes (subset of the spec that the hub produces).
const (
	CodeBadRequest         Code = "bad_request"
	CodeUnauthorized       Code = "unauthorized"
	CodeForbidden          Code = "forbidden"
	CodeNotFound           Code = "not_found"
	CodeConflict           Code = "conflict"
	CodeChildMode          Code = "child_mode"
	CodeRateLimited        Code = "rate_limited"
	CodeInternal           Code = "internal"
	CodeDeviceRevoked      Code = "device_revoked"
	CodeInvalidCredentials Code = "invalid_credentials"
	CodePairingExpired     Code = "pairing_expired"
	CodeSaveConflict       Code = "save_conflict"
	CodeSaveConflictStale  Code = "save_conflict_stale"
	CodePayloadTooLarge    Code = "payload_too_large"
	CodeSessionNotFound    Code = "session_not_found"
	CodeSessionForbidden   Code = "session_forbidden"
	CodeSessionFull        Code = "session_full"
	CodeSessionEnded       Code = "session_ended"
	CodeCapabilityMissing  Code = "capability_missing"
	CodeInviteInvalid      Code = "invite_invalid"
	CodeDisplayNameTaken   Code = "display_name_taken"
	CodeUserDisabled       Code = "user_disabled"
	CodeUploadsDisabled    Code = "uploads_disabled"
	CodeSaveSlotExists     Code = "save_slot_exists"
	CodeSessionRunning     Code = "session_running"
	CodeSaveSlotProtected  Code = "save_slot_protected"
	// Accounts (0.18 package 4).
	CodeConfirmationRequired Code = "confirmation_required"
	CodePasswordRequired     Code = "password_required"
	CodePasswordNotSet       Code = "password_not_set"
)

// Error is a domain error with a spec code. errors.Is compares the code only.
type Error struct {
	Code    Code
	Message string
	// RetryAfter is set on rate_limited answers that know when the next try is possible (seconds, 0 = unknown).
	RetryAfter int
	// ExistingGameID is set on a duplicate ROM upload (conflict) and names the library entry that holds it.
	ExistingGameID string
	// UsedBy is set on tool_in_use answers: what still pins the tool version.
	UsedBy *ToolUsage
}

func (e *Error) Error() string { return fmt.Sprintf("%s: %s", e.Code, e.Message) }

// Is compares errors by code so that errors.Is(err, ErrNotFound) also matches with a custom message.
func (e *Error) Is(target error) bool {
	t, ok := target.(*Error)
	return ok && t.Code == e.Code
}

// Sentinel errors for errors.Is.
var (
	ErrBadRequest         = &Error{Code: CodeBadRequest, Message: "Invalid request"}
	ErrUnauthorized       = &Error{Code: CodeUnauthorized, Message: "Token missing or invalid"}
	ErrForbidden          = &Error{Code: CodeForbidden, Message: "Not allowed"}
	ErrNotFound           = &Error{Code: CodeNotFound, Message: "Not found"}
	ErrConflict           = &Error{Code: CodeConflict, Message: "Conflict"}
	ErrRateLimited        = &Error{Code: CodeRateLimited, Message: "Too many requests"}
	ErrDeviceRevoked      = &Error{Code: CodeDeviceRevoked, Message: "Device has been revoked"}
	ErrInvalidCredentials = &Error{Code: CodeInvalidCredentials, Message: "Invalid credentials"}
	ErrPairingExpired     = &Error{Code: CodePairingExpired, Message: "Pairing request expired"}
	// ErrSaveConflictStale: expected_revision is stale or the conflict is already resolved (nothing changed).
	ErrSaveConflictStale = &Error{Code: CodeSaveConflictStale, Message: "Slot changed since expected_revision; re-read the slot"}
	// ErrPayloadTooLarge: the upload exceeds MaxSaveBytes.
	ErrPayloadTooLarge  = &Error{Code: CodePayloadTooLarge, Message: "Save exceeds 64 MiB"}
	ErrSessionNotFound  = &Error{Code: CodeSessionNotFound, Message: "Session not found"}
	ErrSessionForbidden = &Error{Code: CodeSessionForbidden, Message: "Not allowed for this Session"}
	ErrSessionFull      = &Error{Code: CodeSessionFull, Message: "Session already has the maximum number of viewers"}
	ErrSessionEnded     = &Error{Code: CodeSessionEnded, Message: "Session has ended"}
	// ErrAdminExists: an admin already exists (code conflict).
	ErrAdminExists = &Error{Code: CodeConflict, Message: "An admin already exists"}
	// Invites, user state and uploads.
	ErrInviteInvalid    = &Error{Code: CodeInviteInvalid, Message: "Invite code is invalid, expired, used or revoked"}
	ErrDisplayNameTaken = &Error{Code: CodeDisplayNameTaken, Message: "Display name is already taken"}
	ErrUserDisabled     = &Error{Code: CodeUserDisabled, Message: "User has been disabled"}
	ErrUploadsDisabled  = &Error{Code: CodeUploadsDisabled, Message: "Uploads by users are disabled on this server"}
	// ErrSaveSlotExists: a slot with that name already exists (409).
	ErrSaveSlotExists = &Error{Code: CodeSaveSlotExists, Message: "A slot with this name already exists"}
	// ErrSessionRunning: a Session of the same user runs the game, so its slots cannot be uploaded to, renamed or deleted (409).
	ErrSessionRunning = &Error{Code: CodeSessionRunning, Message: "A Session of this game is running; end it first"}
	// ErrSaveSlotProtected: the default slot cannot be renamed (409).
	ErrSaveSlotProtected = &Error{Code: CodeSaveSlotProtected, Message: "The default slot cannot be renamed"}
	// ErrConfirmationRequired: a dangerous step needs a fresh "Confirm it's you" on this device (403).
	ErrConfirmationRequired = &Error{Code: CodeConfirmationRequired, Message: "Confirm it's you first"}
	// ErrPasswordRequired: the password cannot be removed (admin, or the server requires passwords) (403).
	ErrPasswordRequired = &Error{Code: CodePasswordRequired, Message: "A password is required for this account"}
	// ErrPasswordNotSet: the step needs a password and the account has none (409).
	ErrPasswordNotSet = &Error{Code: CodePasswordNotSet, Message: "This account has no password"}
)

func badRequest(format string, a ...any) *Error {
	return &Error{Code: CodeBadRequest, Message: fmt.Sprintf(format, a...)}
}

func conflict(msg string) *Error { return &Error{Code: CodeConflict, Message: msg} }

func internal(err error) error { return fmt.Errorf("hub: %w", err) }

// Core packages (0.2).
const (
	CodeCorePackageNotFound  Code = "core_package_not_found"
	CodeCoreFileNotAvailable Code = "core_file_not_available"
)

var (
	ErrCorePackageNotFound  = &Error{Code: CodeCorePackageNotFound, Message: "Core package not found"}
	ErrCoreFileNotAvailable = &Error{Code: CodeCoreFileNotAvailable, Message: "Core file not available"}
)

// Tools and variants (0.21, ADR 0031).
const (
	CodeToolInUse          Code = "tool_in_use"
	CodeToolHashMismatch   Code = "tool_hash_mismatch"
	CodeNameTaken          Code = "name_taken"
	CodeOwnVariantMismatch Code = "own_variant_mismatch"
	CodeOwnVariantsOff     Code = "own_variants_off"
)
