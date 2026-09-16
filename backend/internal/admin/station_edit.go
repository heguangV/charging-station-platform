package admin

import (
	"context"
	"fmt"
	"strings"
)

// Station editing.
//
// The create path already existed; this is the change path beside it, and it
// deliberately edits only the four fields an operator can get wrong by hand:
// the name, the address and the two coordinates. Everything else about a
// station is either derived (its charger count), identity (its code, which the
// audit trail and the orders reference) or a separate decision with its own
// endpoint and its own state machine (its status).
//
// Restricting the surface is the point. A "PUT everything" would let a caller
// rewrite the code, which would silently break the references the audit trail
// keeps, and would reach the status, which has a transition rule and must stay
// on the endpoint that enforces it.

// ErrInvalidStationProfile reports an edit outside the contract bounds.
//
// It is separate from ErrInvalidStationFilter because the two describe
// different things to whoever is reading the log: a filter is a query the
// platform will not run, a profile is a change it will not make.
var ErrInvalidStationProfile = fmt.Errorf("admin: invalid station profile")

// UpdateStationCommand carries a validated station profile change.
type UpdateStationCommand struct {
	AdminID     int64
	StationID   int64
	Name        string
	Address     string
	LatitudeE6  int64
	LongitudeE6 int64
	TraceID     string
}

// UpdateStation replaces a station's editable profile under audit.
//
// The bounds are the create path's own, so a station cannot be created with a
// name the platform would later refuse to keep: an edit that accepted what
// creation rejected would make the two disagree about what a valid station is.
func (s *Service) UpdateStation(ctx context.Context, command UpdateStationCommand) (StationRecord, error) {
	if command.AdminID < 1 {
		return StationRecord{}, ErrInvalidAdminActor
	}
	if command.StationID < 1 {
		return StationRecord{}, ErrStationNotFound
	}
	command.Name = strings.TrimSpace(command.Name)
	command.Address = strings.TrimSpace(command.Address)
	if len([]rune(command.Name)) < 1 || len([]rune(command.Name)) > 100 {
		return StationRecord{}, fmt.Errorf("%w: station name length", ErrInvalidStationProfile)
	}
	if len([]rune(command.Address)) < 1 || len([]rune(command.Address)) > 255 {
		return StationRecord{}, fmt.Errorf("%w: station address length", ErrInvalidStationProfile)
	}
	if command.LatitudeE6 < -90_000_000 || command.LatitudeE6 > 90_000_000 ||
		command.LongitudeE6 < -180_000_000 || command.LongitudeE6 > 180_000_000 {
		return StationRecord{}, fmt.Errorf("%w: coordinates out of range", ErrInvalidStationProfile)
	}
	return s.store.UpdateStation(ctx, command)
}
