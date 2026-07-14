#pragma once

#include <cstdint>

namespace primechain::protocol {

struct ValidatorEligibilityPolicyV0 {
    std::uint64_t min_work_score{100};
    std::uint64_t min_reserve_micro_units{5'000'000};
    std::uint64_t endpoint_observation_window{100};
    std::uint64_t endpoint_required_uptime_bps{8000};
    std::uint64_t admission_quorum_numerator{2};
    std::uint64_t admission_quorum_denominator{3};
    std::uint64_t epoch_length{1000};
    std::uint64_t activation_delay_epochs{1};
    std::uint64_t exit_delay_epochs{1};
    std::uint64_t reserve_unlock_delay_epochs{2};
};

struct ValidatorWorkStatsV0 {
    std::uint64_t prime_records_mined{0};
    std::uint64_t composite_records_mined{0};
    std::uint64_t discovery_micro_units{0};
};

inline std::uint64_t validatorWorkScoreV0(const ValidatorWorkStatsV0& stats) {
    return 10 * stats.prime_records_mined +
           2 * stats.composite_records_mined +
           stats.discovery_micro_units / 100000;
}

inline bool validatorMeetsWorkMinimumV0(
    const ValidatorWorkStatsV0& stats,
    const ValidatorEligibilityPolicyV0& policy = ValidatorEligibilityPolicyV0{}) {
    return validatorWorkScoreV0(stats) >= policy.min_work_score;
}

inline bool validatorMeetsReserveMinimumV0(
    std::uint64_t locked_micro_units,
    const ValidatorEligibilityPolicyV0& policy = ValidatorEligibilityPolicyV0{}) {
    return locked_micro_units >= policy.min_reserve_micro_units;
}

inline bool validatorMeetsEndpointUptimeMinimumV0(
    std::uint64_t successful_observations,
    std::uint64_t total_observations,
    const ValidatorEligibilityPolicyV0& policy = ValidatorEligibilityPolicyV0{}) {
    if (total_observations < policy.endpoint_observation_window) return false;
    return successful_observations * 10000 >= total_observations * policy.endpoint_required_uptime_bps;
}

} // namespace primechain::protocol
