#pragma once

#include <loom/cross_cat.hpp>

namespace loom
{

class ScoreServer
{
public:

    typedef protobuf::Post::Score::Query Query;
    typedef protobuf::Post::Score::Result Result;
    typedef CrossCat::Value Value;

    ScoreServer (const CrossCat & cross_cat) :
        cross_cat_(cross_cat),
        partial_values_(),
        scores_(),
        timer_()
    {
    }

    void score_row (
            rng_t & rng,
            const Query & query,
            Result & result);


private:

    const CrossCat & cross_cat_;
    std::vector<Value> partial_values_;
    VectorFloat scores_;
    Timer timer_;
};

inline void ScoreServer::score_row (
        rng_t & rng,
        const Query & query,
        Result & result)
{
    Timer::Scope timer(timer_);

    result.Clear();
    result.set_id(query.id());
    if (not cross_cat_.schema.is_valid(query.data())) {
        result.set_error("invalid query data");
        return;
    }

    cross_cat_.value_split(query.data(), partial_values_);

    const size_t kind_count = cross_cat_.kinds.size();
    for (size_t i = 0; i < kind_count; ++i) {
        const Value & value = partial_values_[i];
        auto & kind = cross_cat_.kinds[i];
        const ProductModel & model = kind.model;
        auto & mixture = kind.mixture;

        mixture.score_value(model, value, scores_, rng);
    }
    result.set_score(distributions::log_sum_exp(scores_));

}

} // namespace loom
