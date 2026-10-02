#pragma once
#include <random>
#include "inference/model.hpp"

namespace pubg_vision::inference {
class MockModel final : public Model {
public:
    explicit MockModel(ModelSpec spec = {}, std::uint32_t seed = 42);
    [[nodiscard]] const ModelSpec& spec() const noexcept override { return spec_; }
    [[nodiscard]] std::string_view backend_name() const noexcept override { return "mock"; }
    [[nodiscard]] std::vector<Candidate> run(const PreparedInput& input, std::stop_token stop = {}) override;
private:
    ModelSpec spec_;
    std::mt19937 random_;
};
} // namespace pubg_vision::inference
