package station

// Status lifecycle for stations and chargers (admin status changes).
//
// The management UI can take a station or a charger out of service, and an
// operator reaching for that switch needs the platform to say plainly whether
// the change is allowed. This file is the whole policy in one place, so the
// answer does not depend on which handler asked.
//
// Station
//
//	OPEN     -> CLOSED      close for business
//	OPEN     -> DISABLED    take the station out of service
//	CLOSED   -> OPEN        reopen
//	CLOSED   -> DISABLED    take the station out of service
//	DISABLED -> OPEN        bring it back
//	DISABLED -> CLOSED      rejected: a station that was taken out of service
//	                        must come back through OPEN, so the operator has to
//	                        say "this station serves customers again" explicitly
//
// Charger
//
//	IDLE       -> DISABLED  take the charger out of service
//	DISABLED   -> IDLE      put it back in the allocation pool
//	FAULT      -> IDLE      a technician cleared it
//	FAULT      -> DISABLED  take it out of service for good
//	OCCUPIED   -> *         rejected: a charger in use must be force-released
//	                        first (/admin/chargers/{id}/release), otherwise the
//	                        active order keeps a charger that is no longer
//	                        available
//	RESTARTING -> *         rejected: a restart command is in flight; the
//	                        receipt decides where the charger lands
//
// In both cases a change to the status the row already has is rejected rather
// than reported as success: the caller asked for a transition, and nothing
// transitioned.

// stationTransitions lists the statuses each station status may move to.
var stationTransitions = map[string]map[string]bool{
	StatusOpen:     {StatusClosed: true, StatusDisabled: true},
	StatusClosed:   {StatusOpen: true, StatusDisabled: true},
	StatusDisabled: {StatusOpen: true},
}

// chargerTransitions lists the statuses each charger status may move to.
var chargerTransitions = map[string]map[string]bool{
	ChargerStatusIdle:     {ChargerStatusDisabled: true},
	ChargerStatusDisabled: {ChargerStatusIdle: true},
	ChargerStatusFault:    {ChargerStatusIdle: true, ChargerStatusDisabled: true},
}

// IsStationStatus reports whether the value is one of the contract's station
// statuses.
func IsStationStatus(status string) bool {
	switch status {
	case StatusOpen, StatusClosed, StatusDisabled:
		return true
	default:
		return false
	}
}

// IsChargerStatus reports whether the value is one of the contract's charger
// statuses.
func IsChargerStatus(status string) bool {
	return chargerStatuses[status]
}

// CanChangeStationStatus reports whether from may move to to.
func CanChangeStationStatus(from, to string) bool {
	return stationTransitions[from][to]
}

// CanChangeChargerStatus reports whether from may move to to.
func CanChangeChargerStatus(from, to string) bool {
	return chargerTransitions[from][to]
}
