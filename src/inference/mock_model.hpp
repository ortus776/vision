#pragma once
#include <random>
#include "inference/model.hpp"

namespace pubg_vision::inference {
class MockModel final : public Model {
public:
    explicit MockModel(ModelSpec spec = {}, std::uint32_t seed = 42);
    [[nodiscard]] const ModelSpec& spec() const noexcept override { return spec_; }
    [[nodiscard]] std::vector<Candidate> run(const PreparedInput& input) override;
private:
    ModelSpec spec_;
    std::mt19937 random_;
};
} // namespace pubg_vision::inference
