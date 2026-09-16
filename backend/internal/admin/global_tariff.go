package admin

import (
	"context"
	"fmt"
	"strings"
)

// The fleet-wide tariff.
//
// The platform's prices live on chargers, not on a tariff table: a charger
// carries an electricity price, a service price and an optional time-of-use
// window. The per-charger endpoint edits one device; this is the fleet-wide
// view and the fleet-wide change beside it, because an operator changing a
// rate for the whole platform should not have to walk a list and hope they
// reached every device.
//
// It is deliberately not the old "base price version" model. That one was
// scoped to a district code and carried an effective time window, which is a
// pricing-version subsystem: it needs a version table, an effective-time
// resolver and a scheduled rollover. This is the smaller thing that is actually
// usable today - read the current prices, set them everywhere, record who did
// it - and it reuses the columns that already exist rather than introducing a
// second source of truth about what a charge costs.
//
// Running orders are unaffected either way: the order domain snapshots the
// tariff when charging starts, so a change applies to the next charge and never
// retroactively to a bill.

// GlobalTariff is one tariff configuration, and how many chargers share it.
type GlobalTariff struct {
	ElectricityPriceCent int64  `json:"electricityPriceCentPerKwh"`
	ServicePriceCent     int64  `json:"servicePriceCentPerKwh"`
	OffPeakPriceCent     *int64 `json:"offPeakElectricityPriceCentPerKwh,omitempty"`
	OffPeakStartHour     *int16 `json:"offPeakStartHour,omitempty"`
	OffPeakEndHour       *int16 `json:"offPeakEndHour,omitempty"`
	ChargerCount         int64  `json:"chargerCount"`
}

// GlobalTariffView is the fleet-wide tariff picture.
//
// The range is reported alongside the configurations because an operator about
// to overwrite every price needs to see what they are overwriting: a fleet
// where every charger already agrees is a no-op, and a fleet spread across a
// wide range is a change worth pausing over.
type GlobalTariffView struct {
	ChargerCount            int64          `json:"chargerCount"`
	ConfigurationCount      int64          `json:"configurationCount"`
	MinElectricityPriceCent int64          `json:"minElectricityPriceCentPerKwh"`
	MaxElectricityPriceCent int64          `json:"maxElectricityPriceCentPerKwh"`
	MinServicePriceCent     int64          `json:"minServicePriceCentPerKwh"`
	MaxServicePriceCent     int64          `json:"maxServicePriceCentPerKwh"`
	OffPeakChargerCount     int64          `json:"offPeakChargerCount"`
	FlatChargerCount        int64          `json:"flatChargerCount"`
	Configurations          []GlobalTariff `json:"configurations"`
}

// GlobalTariffUpdate is a validated fleet-wide tariff change.
//
// The update carries the complete tariff, not a patch. Absent off-peak fields
// mean the fleet becomes flat, which is the same rule the per-charger endpoint
// already follows: after the call every charger holds exactly the tariff that
// was sent, and an operator never has to reason about which devices were
// touched and which were left behind.
type GlobalTariffUpdate struct {
	AdminID              int64
	ElectricityPriceCent int64
	ServicePriceCent     int64
	OffPeakPriceCent     *int64
	OffPeakStartHour     *int16
	OffPeakEndHour       *int16
	Reason               string
	TraceID              string
}

// GlobalTariffResult is what a fleet-wide change reports back.
type GlobalTariffResult struct {
	// AffectedChargers is how many devices now carry the new tariff.
	AffectedChargers int64 `json:"affectedChargers"`
	// PreviousConfigurations is how many distinct tariffs the change replaced.
	// One means the fleet already agreed and nothing of substance changed.
	PreviousConfigurations int64  `json:"previousConfigurations"`
	ElectricityPriceCent   int64  `json:"electricityPriceCentPerKwh"`
	ServicePriceCent       int64  `json:"servicePriceCentPerKwh"`
	OffPeakPriceCent       *int64 `json:"offPeakElectricityPriceCentPerKwh,omitempty"`
	OffPeakStartHour       *int16 `json:"offPeakStartHour,omitempty"`
	OffPeakEndHour         *int16 `json:"offPeakEndHour,omitempty"`
}

// GlobalTariff returns the fleet-wide tariff picture.
func (s *Service) GlobalTariff(ctx context.Context) (GlobalTariffView, error) {
	configurations, err := s.store.GlobalTariff(ctx)
	if err != nil {
		return GlobalTariffView{}, err
	}
	return summarizeTariffs(configurations), nil
}

// UpdateGlobalTariff applies one tariff to every charger, under audit.
//
// The reason is required and audited: this is the single most consequential
// write on the boundary - it changes what every charge costs - and "who set
// the platform's price to this, and why" has to be answerable afterwards.
func (s *Service) UpdateGlobalTariff(ctx context.Context, update GlobalTariffUpdate) (GlobalTariffResult, error) {
	if update.AdminID < 1 {
		return GlobalTariffResult{}, ErrInvalidAdminActor
	}
	if update.ElectricityPriceCent < 0 || update.ServicePriceCent < 0 {
		return GlobalTariffResult{}, fmt.Errorf("%w: prices must not be negative", ErrInvalidTariff)
	}
	update.Reason = strings.TrimSpace(update.Reason)
	if len([]rune(update.Reason)) < 2 || len([]rune(update.Reason)) > 200 {
		return GlobalTariffResult{}, fmt.Errorf("%w: reason length", ErrInvalidReason)
	}
	// The off-peak triple is all-or-nothing, exactly as on the per-charger
	// endpoint: a partial window would leave a tariff with a dangling boundary.
	offPeakFields := 0
	for _, present := range []bool{
		update.OffPeakPriceCent != nil,
		update.OffPeakStartHour != nil,
		update.OffPeakEndHour != nil,
	} {
		if present {
			offPeakFields++
		}
	}
	switch offPeakFields {
	case 0:
	case 3:
		if *update.OffPeakPriceCent < 0 {
			return GlobalTariffResult{}, fmt.Errorf("%w: off-peak price must not be negative", ErrInvalidTariff)
		}
		if *update.OffPeakStartHour < 0 || *update.OffPeakStartHour > 23 ||
			*update.OffPeakEndHour < 0 || *update.OffPeakEndHour > 23 ||
			*update.OffPeakStartHour == *update.OffPeakEndHour {
			return GlobalTariffResult{}, fmt.Errorf("%w: off-peak window", ErrInvalidTariff)
		}
	default:
		return GlobalTariffResult{}, fmt.Errorf("%w: the off-peak window must be complete or absent", ErrInvalidTariff)
	}
	return s.store.UpdateGlobalTariff(ctx, update)
}

// summarizeTariffs folds the stored configurations into the fleet picture.
func summarizeTariffs(configurations []GlobalTariff) GlobalTariffView {
	view := GlobalTariffView{Configurations: configurations}
	if view.Configurations == nil {
		view.Configurations = []GlobalTariff{}
	}
	view.ConfigurationCount = int64(len(configurations))
	for index, configuration := range configurations {
		view.ChargerCount += configuration.ChargerCount
		if configuration.OffPeakPriceCent == nil {
			view.FlatChargerCount += configuration.ChargerCount
		} else {
			view.OffPeakChargerCount += configuration.ChargerCount
		}
		if index == 0 {
			view.MinElectricityPriceCent = configuration.ElectricityPriceCent
			view.MaxElectricityPriceCent = configuration.ElectricityPriceCent
			view.MinServicePriceCent = configuration.ServicePriceCent
			view.MaxServicePriceCent = configuration.ServicePriceCent
			continue
		}
		view.MinElectricityPriceCent = min64(view.MinElectricityPriceCent, configuration.ElectricityPriceCent)
		view.MaxElectricityPriceCent = max64(view.MaxElectricityPriceCent, configuration.ElectricityPriceCent)
		view.MinServicePriceCent = min64(view.MinServicePriceCent, configuration.ServicePriceCent)
		view.MaxServicePriceCent = max64(view.MaxServicePriceCent, configuration.ServicePriceCent)
	}
	return view
}

func min64(left, right int64) int64 {
	if left < right {
		return left
	}
	return right
}

func max64(left, right int64) int64 {
	if left > right {
		return left
	}
	return right
}
