#pragma once

// Read-only export surfaces for supervision systems.
//
// The monitor never opens a listening socket: a billing channel should not
// grow an attack surface for the sake of a dashboard. Instead it writes plain
// files atomically, and whatever already supervises the site (a static web
// server, node_exporter's textfile collector, a cron job) reads them.
//
// prometheus_metrics() renders one bay in the Prometheus text exposition
// format 0.0.4. Every value is built from the exact integer registers with
// integer fixed-point formatting; no floating point is involved. A quantity the
// monitor cannot know is OMITTED, never exported as zero: absent is the honest
// way to say "undefined" in Prometheus.
//
// Families (all carry the label asset="<asset_id>"):
//   charger_info{version,currency}                     gauge   always 1
//   charger_export_timestamp_seconds                   gauge   Unix time the file was rendered; omitted when
//                                                              the caller passes none. node_exporter keeps serving a
//                                                              dead monitor's last file, so alert on this, not on
//                                                              the other values
//   charger_state{state}                               gauge   one-hot over UNKNOWN OFF READY CHARGING FAULT
//                                                              DISCONNECTED
//   charger_power_enabled                              gauge   0/1, power stage status
//   charger_measurement_voltage_volts                  gauge   only while the measurement stream is fresh
//   charger_measurement_current_amperes                gauge   only while the measurement stream is fresh
//   charger_measurement_power_watts                    gauge   only while fresh; 0 unless CHARGING
//   charger_measurements_stale / charger_status_stale  gauge   0/1, stream older than its timeout
//   charger_measurement_age_seconds                    gauge   omitted before the first accepted frame
//   charger_status_age_seconds                         gauge   omitted before the first accepted frame
//   charger_frames_accepted_total{type}                counter type = measurement | status
//   charger_frames_rejected_total                      counter
//   charger_sessions_opened_total                      counter
//   charger_sessions_closed_total                      counter
//   charger_sessions_billable_total                    counter billing_complete sessions only
//   charger_sessions_undersampled_total                counter
//   charger_integration_gaps_total                     counter dropouts that were NOT interpolated
//   charger_energy_closed_watt_hours_total             counter exact, six decimals (microwatt-hours)
//   charger_state_seconds_total{state}                 counter dwell time per state
//   charger_in_service_seconds_total                   counter
//   charger_availability_ratio / _utilization_ratio    gauge   0..1, omitted while undefined (-1 ppm)
//   charger_frame_accept_ratio                         gauge   0..1, omitted before the first frame
//   charger_fault_events_total                         counter
//   charger_timeout_events_total                       counter
//   charger_transport_errors_total                     counter
//   charger_diagnostic_active{diagnostic}              gauge   measurement_stuck voltage_rate_exceeded
//                                                              current_rate_exceeded voltage_sag current_hunting
//   charger_diagnostic_warnings_total{diagnostic}      counter measurement_stuck rate_of_change voltage_sag
//                                                              current_hunting
//   charger_fault_active{code}                         gauge   present only while a fault is latched
//   charger_session_active                             gauge   0/1
//   charger_session_energy_watt_hours                  gauge   present only while a session is open
//   charger_session_duration_seconds                   gauge   present only while a session is open
//
// A diagnostic is an advisory. Exporting it never blocks charging.

#include <string>

#include "charger_monitor.hpp"
#include "charger_portfolio.hpp"

namespace charger {

std::string prometheus_metrics(const Snapshot& snapshot, const std::string& version = VERSION,
                               std::int64_t unix_seconds = -1);

// Fleet rollup rebuilt from a journal, in the same format (charger_portfolio_*
// and charger_asset_* families). These are gauges: the journal is the source of
// truth and a rotated journal legitimately restarts from zero, so they are not
// counters. Revenue is in EUR excluding VAT, exact to six decimals. Undefined
// ratios are omitted.
std::string prometheus_portfolio(const PortfolioKpi& kpi);

// Escapes a label value per the exposition format: backslash, double quote and
// newline.
std::string prometheus_escape_label(const std::string& value);

// Replaces `path` atomically: the content is written to `path`.tmp in the same
// directory, flushed, then renamed over the target, so a reader sees either the
// old file or the new one, never a partial file. `durable` additionally fsyncs
// the temporary file before the rename; status files that live on tmpfs do not
// need it, and flash storage does not want a sync per second. On failure the
// temporary file is removed and `error` says why.
bool write_file_atomic(const std::string& path, const std::string& content, std::string& error, bool durable = true);

}  // namespace charger
