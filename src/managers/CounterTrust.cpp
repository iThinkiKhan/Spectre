
#include "CounterTrust.h"

const char* counterTrustText(CounterTrust state) {
    switch (state) {
        case CounterTrust::TrustedSnapshotLagged: return "trusted_snapshot_lagged";
        case CounterTrust::Degraded:              return "degraded";
        case CounterTrust::RepairRequired:        return "repair_required";
        case CounterTrust::EmergencyOnly:         return "emergency_only";
        case CounterTrust::Trusted:
        default:                                  return "trusted";
    }
}
