#include "loom.hpp"
#include "schedules.hpp"
#include "infer_grid.hpp"
#include "protobuf.hpp"

namespace loom
{

using ::distributions::sample_from_scores_overwrite;

//----------------------------------------------------------------------------
// StreamInterval

class StreamInterval : noncopyable
{
public:

    template<class RemoveRow>
    StreamInterval (
            const char * rows_in,
            Assignments & assignments,
            RemoveRow remove_row) :
        unassigned_(rows_in),
        assigned_(rows_in)
    {
        LOOM_ASSERT(assigned_.is_file(), "only files support StreamInterval");

        if (assignments.row_count()) {
            protobuf::SparseRow row;

            // point unassigned at first unassigned row
            const auto last_assigned_rowid = assignments.rowids().back();
            do {
                read_unassigned(row);
            } while (row.id() != last_assigned_rowid);

            // point rows_assigned at first assigned row
            const auto first_assigned_rowid = assignments.rowids().front();
            do {
                read_assigned(row);
            } while (row.id() != first_assigned_rowid);
            remove_row(row);
        }
    }

    void read_unassigned (protobuf::SparseRow & row)
    {
        unassigned_.cyclic_read_stream(row);
    }

    void read_assigned (protobuf::SparseRow & row)
    {
        assigned_.cyclic_read_stream(row);
    }

private:

    protobuf::InFile unassigned_;
    protobuf::InFile assigned_;
};

//----------------------------------------------------------------------------
// Loom

Loom::Loom (
        rng_t & rng,
        const char * model_in,
        const char * groups_in,
        const char * assign_in,
        size_t empty_group_count,
        size_t algorithm8_parallel) :
    empty_group_count_(empty_group_count),
    cross_cat_(),
    algorithm8_(),
    assignments_(),
    value_join_(cross_cat_),
    unobserved_(),
    partial_values_(),
    scores_(),
    algorithm8_queues_(),
    algorithm8_workers_(),
    algorithm8_parallel_(algorithm8_parallel)
{
    timers_["total"].start();
    LOOM_ASSERT_LT(0, empty_group_count_);
    cross_cat_.model_load(model_in);
    const size_t kind_count = cross_cat_.kinds.size();
    LOOM_ASSERT(kind_count, "no kinds, loom is empty");
    assignments_.init(kind_count);
    partial_values_.resize(kind_count);
    size_t feature_count = cross_cat_.schema.total_size();
    for (size_t f = 0; f < feature_count; ++f) {
        unobserved_.add_observed(false);
    }

    if (groups_in) {
        cross_cat_.mixture_load(groups_in, empty_group_count, rng);
    } else {
        cross_cat_.mixture_init_empty(empty_group_count, rng);
    }

    if (assign_in) {
        assignments_.load(assign_in);
        for (const auto & kind : cross_cat_.kinds) {
            LOOM_ASSERT_LE(
                assignments_.row_count(),
                kind.mixture.clustering.sample_size());
        }
    }

    validate();
}

//----------------------------------------------------------------------------
// High level operations

void Loom::dump (
        const char * model_out,
        const char * groups_out,
        const char * assign_out) const
{
    if (model_out) {
        cross_cat_.model_dump(model_out);
    }

    if (groups_out or assign_out) {
        std::vector<std::vector<uint32_t>> sorted_to_globals =
            cross_cat_.get_sorted_groupids();

        if (groups_out) {
            cross_cat_.mixture_dump(groups_out, sorted_to_globals);
        }

        if (assign_out) {
            assignments_.dump(assign_out, sorted_to_globals);
        }
    }
}

void Loom::infer_single_pass (
        rng_t & rng,
        const char * rows_in,
        const char * assign_out)
{
    protobuf::InFile rows(rows_in);
    protobuf::SparseRow row;

    if (assign_out) {

        protobuf::OutFile assignments(assign_out);
        protobuf::Assignment assignment;

        while (rows.try_read_stream(row)) {
            add_row(rng, row, assignment);
            assignments.write_stream(assignment);
        }

    } else {

        while (rows.try_read_stream(row)) {
            add_row_noassign(rng, row);
        }
    }
}

class Loom::Algorithm8Kernel
{
public:

    struct Status
    {
        size_t total_count;
        size_t change_count;
    };

    Algorithm8Kernel (
            Loom & loom,
            bool init_cache,
            size_t ephemeral_kind_count,
            size_t iterations,
            size_t max_reject_iters,
            rng_t & rng) :
        loom_(loom),
        init_cache_(init_cache),
        ephemeral_kind_count_(ephemeral_kind_count),
        iterations_(iterations),
        max_reject_iters_(max_reject_iters),
        reject_iters_(0),
        rng_(rng)
    {
        LOOM_ASSERT_LT(0, max_reject_iters);
        reset_status();
        Timer::Scope timer(loom_.timers_["algo8"]);
        loom_.prepare_algorithm8(ephemeral_kind_count_, rng_);
    }

    ~Algorithm8Kernel ()
    {
        Timer::Scope timer(loom_.timers_["algo8"]);
        loom_.cleanup_algorithm8(rng_);
    }

    void run ()
    {
        Timer::Scope timer(loom_.timers_["algo8"]);
        loom_.algorithm8_queues_.producer_wait();

        size_t change_count = loom_.run_algorithm8(
            ephemeral_kind_count_,
            iterations_,
            init_cache_,
            rng_);

        if (change_count > 0) {
            reject_iters_ = 0;
        } else {
            ++reject_iters_;
        }

        status_.total_count += loom_.cross_cat_.featureid_to_kindid.size();
        status_.change_count += change_count;
    }

    bool is_mixing () const { return reject_iters_ < max_reject_iters_; }

    const Status & status () const { return status_; }

    void reset_status ()
    {
        Status zero = {0, 0};
        status_ = zero;
    }

private:

    Loom & loom_;
    const bool init_cache_;
    const size_t ephemeral_kind_count_;
    const size_t iterations_;
    const size_t max_reject_iters_;
    size_t reject_iters_;
    rng_t & rng_;
    Status status_;
};

void Loom::log_iter_metrics (size_t iter, Algorithm8Kernel * kernel)
{
    if (global_logger) {
        protobuf::InferLog message;
        auto & args = * message.mutable_args();

        args.set_iter(iter);

        for (auto & pair : timers_) {
            auto & timer = * args.add_timers();
            timer.set_name(pair.first.c_str());
            timer.set_elapsed(pair.second.elapsed());
        }

        auto & summary = * args.mutable_summary();
        auto & kind_hypers = * summary.mutable_kind_hypers();
        auto & model_hypers = * summary.mutable_model_hypers();
        for (const auto & kind : cross_cat_.kinds) {
            summary.add_category_counts(
                kind.mixture.clustering.counts().size());
            summary.add_feature_counts(kind.featureids.size());
            kind_hypers.add_alphas(kind.model.clustering.alpha);
            kind_hypers.add_ds(kind.model.clustering.d);
        }
        model_hypers.set_alpha(cross_cat_.feature_clustering.alpha);
        model_hypers.set_d(cross_cat_.feature_clustering.d);

        auto & scores = * args.mutable_scores();
        scores.set_assigned_object_count(assignments_.row_count());

        if (kernel) {
            auto & kernel_status = * args.mutable_kernel_status();
            auto & algo8 = * kernel_status.mutable_algo8();
            algo8.set_total_count(kernel->status().total_count);
            algo8.set_change_count(kernel->status().change_count);
        }

        global_logger.log(message);
    }
}

void Loom::infer_multi_pass (
        rng_t & rng,
        const char * rows_in,
        double cat_extra_passes,
        double kind_extra_passes,
        size_t ephemeral_kind_count,
        size_t iterations,
        size_t max_reject_iters)
{
    LOOM_ASSERT_LE(0, cat_extra_passes);
    LOOM_ASSERT_LE(0, kind_extra_passes);
    LOOM_ASSERT_LT(0, cat_extra_passes + kind_extra_passes);
    if (kind_extra_passes > 0) {
        LOOM_ASSERT_LT(0, ephemeral_kind_count);
        LOOM_ASSERT_LT(0, iterations);
        LOOM_ASSERT_LT(0, max_reject_iters);
    }

    auto & cat_timer = timers_["cat"];
    auto & hyper_timer = timers_["hyper"];

    typedef BatchedAnnealingSchedule Schedule;
    auto _remove_row = [&](protobuf::SparseRow & row) { remove_row(rng, row); };
    StreamInterval rows(rows_in, assignments_, _remove_row);
    protobuf::SparseRow row;

    size_t tardis_iter = 0;
    log_iter_metrics(tardis_iter++);

    if (kind_extra_passes > 0) {
        bool init_cache = false;
        Algorithm8Kernel kernel(
            * this,
            init_cache,
            ephemeral_kind_count,
            iterations,
            max_reject_iters,
            rng);

        double extra_passes = kind_extra_passes + cat_extra_passes;
        Schedule schedule(extra_passes, assignments_.row_count());
        cat_timer.start();
        for (bool mixing = true; LOOM_LIKELY(mixing);) {
            switch (schedule.next_action()) {

                case Schedule::add:
                    rows.read_unassigned(row);
                    if (LOOM_UNLIKELY(not try_add_row_algorithm8(rng, row))) {
                        cat_timer.stop();
                        return;
                    }
                    break;

                case Schedule::remove:
                    rows.read_assigned(row);
                    remove_row_algorithm8(rng, row);
                    break;

                case Schedule::process_batch:
                    cat_timer.stop();
                    kernel.run();
                    mixing = kernel.is_mixing();
                    hyper_timer.start();
                    cross_cat_.infer_hypers(rng);
                    hyper_timer.stop();
                    cat_timer.start();
                    log_iter_metrics(tardis_iter++, & kernel);
                    kernel.reset_status();
                    break;
            }
        }
    }

    Schedule schedule(cat_extra_passes, assignments_.row_count());
    while (true) {
        switch (schedule.next_action()) {

            case Schedule::add:
                rows.read_unassigned(row);
                if (LOOM_UNLIKELY(not try_add_row(rng, row))) {
                    cat_timer.stop();
                    return;
                }
                break;

            case Schedule::remove:
                rows.read_assigned(row);
                remove_row(rng, row);
                break;

            case Schedule::process_batch:
                cat_timer.stop();
                hyper_timer.start();
                cross_cat_.infer_hypers(rng);
                hyper_timer.stop();
                cat_timer.start();
                log_iter_metrics(tardis_iter++);
                break;
        }
    }
}

void Loom::posterior_enum (
        rng_t & rng,
        const char * rows_in,
        const char * samples_out,
        size_t sample_count,
        size_t sample_skip)
{
    LOOM_ASSERT_LE(1, sample_count);
    LOOM_ASSERT(sample_skip > 0 or sample_count == 1, "zero diversity");
    const auto rows = protobuf_stream_load<protobuf::SparseRow>(rows_in);
    LOOM_ASSERT_LT(0, rows.size());
    protobuf::OutFile sample_stream(samples_out);
    protobuf::PosteriorEnum::Sample sample;

    if (assignments_.rowids().empty()) {
        for (const auto & row : rows) {
            bool adding_data = try_add_row(rng, row);
            LOOM_ASSERT(adding_data, "duplicate row: " << row.id());
        }
    }

    for (size_t i = 0; i < sample_count; ++i) {
        for (size_t t = 0; t < sample_skip; ++t) {
            for (const auto & row : rows) {
                remove_row(rng, row);
                try_add_row(rng, row);
            }
        }
        dump_posterior_enum(sample, rng);
        sample_stream.write_stream(sample);
    }
}

void Loom::posterior_enum (
        rng_t & rng,
        const char * rows_in,
        const char * samples_out,
        size_t sample_count,
        size_t sample_skip,
        size_t ephemeral_kind_count,
        size_t iterations)
{
    LOOM_ASSERT_LE(1, sample_count);
    LOOM_ASSERT(sample_skip > 0 or sample_count == 1, "zero diversity");
    const auto rows = protobuf_stream_load<protobuf::SparseRow>(rows_in);
    LOOM_ASSERT_LT(0, rows.size());
    protobuf::OutFile sample_stream(samples_out);
    protobuf::PosteriorEnum::Sample sample;

    if (assignments_.rowids().empty()) {
        for (const auto & row : rows) {
            bool adding_data = try_add_row(rng, row);
            LOOM_ASSERT(adding_data, "duplicate row: " << row.id());
        }
    }

    bool init_cache = true;
    size_t bogus_max_reject_iters = 1;
    Algorithm8Kernel kernel(
        * this,
        init_cache,
        ephemeral_kind_count,
        iterations,
        bogus_max_reject_iters,
        rng);

    for (size_t i = 0; i < sample_count; ++i) {
        for (size_t t = 0; t < sample_skip; ++t) {
            for (const auto & row : rows) {
                remove_row_algorithm8(rng, row);
                try_add_row_algorithm8(rng, row);
            }

            kernel.run();
        }

        dump_posterior_enum(sample, rng);
        sample_stream.write_stream(sample);
    }
}

void Loom::predict (
        rng_t & rng,
        const char * queries_in,
        const char * results_out)
{
    protobuf::InFile query_stream(queries_in);
    protobuf::OutFile result_stream(results_out);
    protobuf::PreQL::Predict::Query query;
    protobuf::PreQL::Predict::Result result;

    while (query_stream.try_read_stream(query)) {
        predict_row(rng, query, result);
        result_stream.write_stream(result);
        result_stream.flush();
    }
}

//----------------------------------------------------------------------------
// Low level operations

inline void Loom::dump_posterior_enum (
        protobuf::PosteriorEnum::Sample & message,
        rng_t & rng)
{
    float score = cross_cat_.score_data(rng);
    const size_t row_count = assignments_.row_count();
    const size_t kind_count = assignments_.kind_count();
    const auto & rowids = assignments_.rowids();

    message.Clear();
    for (size_t kindid = 0; kindid < kind_count; ++kindid) {
        const auto & kind = cross_cat_.kinds[kindid];
        if (not kind.featureids.empty()) {
            const auto & groupids = assignments_.groupids(kindid);
            auto & message_kind = * message.add_kinds();
            for (auto featureid : kind.featureids) {
                message_kind.add_featureids(featureid);
            }
            std::unordered_map<size_t, std::vector<size_t>> groupids_map;
            for (size_t i = 0; i < row_count; ++i) {
                groupids_map[groupids[i]].push_back(rowids[i]);
            }
            for (const auto & pair : groupids_map) {
                auto & message_group = * message_kind.add_groups();
                for (const auto & rowid : pair.second) {
                    message_group.add_rowids(rowid);
                }
            }
        }
    }
    message.set_score(score);
}

size_t Loom::count_untracked_rows () const
{
    LOOM_ASSERT_LT(0, cross_cat_.kinds.size());
    size_t total_row_count = cross_cat_.kinds[0].mixture.count_rows();
    size_t assigned_row_count = assignments_.row_count();
    LOOM_ASSERT_LE(assigned_row_count, total_row_count);
    return total_row_count - assigned_row_count;
}

void Loom::prepare_algorithm8 (
        size_t ephemeral_kind_count,
        rng_t & rng)
{
    LOOM_ASSERT_LT(0, ephemeral_kind_count);
    LOOM_ASSERT_EQ(count_untracked_rows(), 0);

    init_featureless_kinds(ephemeral_kind_count, rng);
    algorithm8_.model_load(cross_cat_);
    algorithm8_.mixture_init_empty(cross_cat_, rng);
    resize_algorithm8(rng);

    validate();
}

size_t Loom::run_algorithm8 (
        size_t ephemeral_kind_count,
        size_t iterations,
        bool init_cache,
        rng_t & rng)
{
    LOOM_ASSERT_LT(0, ephemeral_kind_count);
    if (LOOM_DEBUG_LEVEL >= 1) {
        auto assigned_row_count = assignments_.row_count();
        auto cross_cat_row_count = cross_cat_.kinds[0].mixture.count_rows();
        auto algorithm8_row_count = algorithm8_.kinds[0].mixture.count_rows();
        LOOM_ASSERT_EQ(assigned_row_count, cross_cat_row_count);
        LOOM_ASSERT_EQ(algorithm8_row_count, cross_cat_row_count);
    }

    validate();

    const auto old_kindids = cross_cat_.featureid_to_kindid;
    auto new_kindids = old_kindids;
    algorithm8_.infer_assignments(
        new_kindids,
        iterations,
        algorithm8_parallel_,
        rng);

    const size_t feature_count = old_kindids.size();
    size_t change_count = 0;
    for (size_t featureid = 0; featureid < feature_count; ++featureid) {
        size_t old_kindid = old_kindids[featureid];
        size_t new_kindid = new_kindids[featureid];
        if (new_kindid != old_kindid) {
            move_feature_to_kind(featureid, new_kindid, init_cache, rng);
            ++change_count;
        }
    }

    init_featureless_kinds(ephemeral_kind_count, rng);
    algorithm8_.mixture_init_empty(cross_cat_, rng);
    resize_algorithm8(rng);

    validate();

    return change_count;
}

void Loom::cleanup_algorithm8 (rng_t & rng)
{
    algorithm8_.clear();
    resize_algorithm8(rng);
    init_featureless_kinds(0, rng);

    validate();
}

void Loom::resize_algorithm8 (rng_t & rng)
{
    algorithm8_queues_.unsafe_set_capacity(algorithm8_parallel_);
    if (not algorithm8_parallel_) {
        return;
    }

    const size_t target_size = algorithm8_.kinds.size();
    LOOM_ASSERT_EQ(algorithm8_queues_.size(), algorithm8_workers_.size());
    const size_t start_size = algorithm8_workers_.size();
    if (target_size == 0) {

        for (size_t k = 0; k < start_size; ++k) {
            algorithm8_queues_.producer_hangup(k);
        }
        for (size_t k = 0; k < start_size; ++k) {
            algorithm8_workers_[k].join();
        }
        algorithm8_queues_.unsafe_resize(0);
        algorithm8_workers_.clear();

    } else if (target_size > start_size) {

        algorithm8_queues_.unsafe_resize(target_size);
        algorithm8_workers_.reserve(target_size);
        for (size_t k = start_size; k < target_size; ++k) {
            rng_t::result_type seed = rng();
            algorithm8_workers_.push_back(
                std::thread(&Loom::algorithm8_work, this, k, seed));
        }

    } else {
        // do not shrink; instead save spare threads for later
    }
}

void Loom::add_featureless_kind (rng_t & rng)
{
    auto & kind = cross_cat_.kinds.packed_add();
    auto & model = kind.model;
    auto & mixture = kind.mixture;
    model.clear();

    const auto & grid_prior = cross_cat_.hyper_prior.inner_prior().clustering();
    if (grid_prior.size()) {
        model.clustering = sample_clustering_prior(grid_prior, rng);
    } else {
        model.clustering = cross_cat_.kinds[0].model.clustering;
    }

    const size_t row_count = assignments_.row_count();
    const std::vector<int> assignment_vector =
        model.clustering.sample_assignments(row_count, rng);
    size_t group_count = 0;
    for (size_t groupid : assignment_vector) {
        group_count = std::max(group_count, 1 + groupid);
    }
    std::vector<int> counts(group_count + empty_group_count_, 0);
    auto & assignments = assignments_.packed_add();
    for (int groupid : assignment_vector) {
        assignments.push(groupid);
        ++counts[groupid];
    }
    mixture.init_unobserved(model, counts, rng);
}

void Loom::remove_featureless_kind (size_t kindid)
{
    LOOM_ASSERT(
        cross_cat_.kinds[kindid].featureids.empty(),
        "cannot remove nonempty kind: " << kindid);

    cross_cat_.kinds.packed_remove(kindid);
    assignments_.packed_remove(kindid);

    // this is simpler than keeping a MixtureIdTracker for kinds
    if (kindid < cross_cat_.kinds.size()) {
        for (auto featureid : cross_cat_.kinds[kindid].featureids) {
            cross_cat_.featureid_to_kindid[featureid] = kindid;
        }
    }
}

inline void Loom::init_featureless_kinds (
        size_t featureless_kind_count,
        rng_t & rng)
{
    for (int i = cross_cat_.kinds.size() - 1; i >= 0; --i) {
        if (cross_cat_.kinds[i].featureids.empty()) {
            remove_featureless_kind(i);
        }
    }

    for (size_t i = 0; i < featureless_kind_count; ++i) {
        add_featureless_kind(rng);
    }

    partial_values_.resize(cross_cat_.kinds.size());

    validate_cross_cat();
}

void Loom::move_feature_to_kind (
        size_t featureid,
        size_t new_kindid,
        bool init_cache,
        rng_t & rng)
{
    size_t old_kindid = cross_cat_.featureid_to_kindid[featureid];
    LOOM_ASSERT_NE(new_kindid, old_kindid);

    CrossCat::Kind & old_kind = cross_cat_.kinds[old_kindid];
    CrossCat::Kind & new_kind = cross_cat_.kinds[new_kindid];
    Algorithm8::Kind & algorithm8_kind = algorithm8_.kinds[new_kindid];

    algorithm8_kind.mixture.move_feature_to(
        featureid,
        old_kind.model, old_kind.mixture,
        new_kind.model, new_kind.mixture,
        init_cache,
        rng);

    old_kind.featureids.erase(featureid);
    new_kind.featureids.insert(featureid);
    cross_cat_.featureid_to_kindid[featureid] = new_kindid;

    validate_cross_cat();
}

inline void Loom::add_row_noassign (
        rng_t & rng,
        const protobuf::SparseRow & row)
{
    cross_cat_.value_split(row.data(), partial_values_);

    const size_t kind_count = cross_cat_.kinds.size();
    for (size_t i = 0; i < kind_count; ++i) {
        const Value & value = partial_values_[i];
        auto & kind = cross_cat_.kinds[i];
        const ProductModel & model = kind.model;
        auto & mixture = kind.mixture;

        mixture.score_value(model, value, scores_, rng);
        size_t groupid = sample_from_scores_overwrite(rng, scores_);
        mixture.add_value(model, groupid, value, rng);
    }
}

inline void Loom::add_row (
        rng_t & rng,
        const protobuf::SparseRow & row,
        protobuf::Assignment & assignment_out)
{
    cross_cat_.value_split(row.data(), partial_values_);
    assignment_out.set_rowid(row.id());
    assignment_out.clear_groupids();

    const size_t kind_count = cross_cat_.kinds.size();
    for (size_t i = 0; i < kind_count; ++i) {
        const Value & value = partial_values_[i];
        auto & kind = cross_cat_.kinds[i];
        const ProductModel & model = kind.model;
        auto & mixture = kind.mixture;

        mixture.score_value(model, value, scores_, rng);
        size_t groupid = sample_from_scores_overwrite(rng, scores_);
        mixture.add_value(model, groupid, value, rng);
        assignment_out.add_groupids(groupid);
    }
}

inline bool Loom::try_add_row (
        rng_t & rng,
        const protobuf::SparseRow & row)
{
    bool already_added = not assignments_.rowids().try_push(row.id());
    if (LOOM_UNLIKELY(already_added)) {
        return false;
    }

    cross_cat_.value_split(row.data(), partial_values_);

    const auto seed = rng();
    const size_t kind_count = cross_cat_.kinds.size();
    //#pragma omp parallel
    {
        rng_t rng;
        //#pragma omp for schedule(static)
        for (size_t i = 0; i < kind_count; ++i) {
            rng.seed(seed + i);
            const Value & value = partial_values_[i];
            auto & kind = cross_cat_.kinds[i];
            const ProductModel & model = kind.model;
            auto & mixture = kind.mixture;

            mixture.score_value(model, value, scores_, rng);
            size_t groupid = sample_from_scores_overwrite(rng, scores_);
            mixture.add_value(model, groupid, value, rng);
            size_t global_groupid =
                mixture.id_tracker.packed_to_global(groupid);
            assignments_.groupids(i).push(global_groupid);
        }
    }

    return true;
}

inline bool Loom::try_add_row_algorithm8 (
        rng_t & rng,
        const protobuf::SparseRow & row)
{
    bool already_added = not assignments_.rowids().try_push(row.id());
    if (LOOM_UNLIKELY(already_added)) {
        return false;
    }

    LOOM_ASSERT_EQ(cross_cat_.kinds.size(), algorithm8_.kinds.size());
    const size_t kind_count = cross_cat_.kinds.size();

    if (algorithm8_parallel_) {

        auto * envelope = algorithm8_queues_.producer_alloc();
        Algorithm8Task & task = envelope->message;
        task.next_action_is_add = true;
        task.full_value = row.data();
        cross_cat_.value_split(task.full_value, task.partial_values);
        algorithm8_queues_.producer_send(envelope, kind_count);

    } else {

        const Value & full_value = row.data();
        cross_cat_.value_split(full_value, partial_values_);
        for (size_t i = 0; i < kind_count; ++i) {
            algorithm8_work_add(i, partial_values_[i], full_value, rng);
        }
    }

    return true;
}

inline void Loom::algorithm8_work_add (
        size_t kindid,
        const Value & partial_value,
        const Value & full_value,
        rng_t & rng)
{
    auto & kind = cross_cat_.kinds[kindid];
    const ProductModel & partial_model = kind.model;
    const ProductModel & full_model = algorithm8_.model;
    auto & partial_mixture = kind.mixture;
    auto & full_mixture = algorithm8_.kinds[kindid].mixture;

    partial_mixture.score_value(partial_model, partial_value, scores_, rng);
    size_t groupid = sample_from_scores_overwrite(rng, scores_);
    partial_mixture.add_value(partial_model, groupid, partial_value, rng);
    full_mixture.add_value(full_model, groupid, full_value, rng);
    size_t global_groupid =
        partial_mixture.id_tracker.packed_to_global(groupid);
    assignments_.groupids(kindid).push(global_groupid);
}

inline void Loom::remove_row (
        rng_t & rng,
        const protobuf::SparseRow & row)
{
    const auto rowid = assignments_.rowids().pop();
    if (LOOM_DEBUG_LEVEL >= 1) {
        LOOM_ASSERT_EQ(rowid, row.id());
    }

    cross_cat_.value_split(row.data(), partial_values_);

    const auto seed = rng();
    const size_t kind_count = cross_cat_.kinds.size();
    //#pragma omp parallel
    {
        rng_t rng;
        //#pragma omp for schedule(static)
        for (size_t i = 0; i < kind_count; ++i) {
            rng.seed(seed + i);
            const Value & value = partial_values_[i];
            auto & kind = cross_cat_.kinds[i];
            const ProductModel & model = kind.model;
            auto & mixture = kind.mixture;

            auto global_groupid = assignments_.groupids(i).pop();
            auto groupid = mixture.id_tracker.global_to_packed(global_groupid);
            mixture.remove_value(model, groupid, value, rng);
        }
    }
}

inline void Loom::remove_row_algorithm8 (
        rng_t & rng,
        const protobuf::SparseRow & row)
{
    const auto rowid = assignments_.rowids().pop();
    if (LOOM_DEBUG_LEVEL >= 1) {
        LOOM_ASSERT_EQ(rowid, row.id());
    }

    LOOM_ASSERT_EQ(cross_cat_.kinds.size(), algorithm8_.kinds.size());
    const size_t kind_count = cross_cat_.kinds.size();

    if (algorithm8_parallel_) {

        auto * envelope = algorithm8_queues_.producer_alloc();
        Algorithm8Task & task = envelope->message;
        task.next_action_is_add = false;
        cross_cat_.value_split(row.data(), task.partial_values);
        algorithm8_queues_.producer_send(envelope, kind_count);

    } else {

        cross_cat_.value_split(row.data(), partial_values_);
        for (size_t i = 0; i < kind_count; ++i) {
            algorithm8_work_remove(i, partial_values_[i], rng);
        }
    }
}

inline void Loom::algorithm8_work_remove (
        size_t kindid,
        const Value & partial_value,
        rng_t & rng)
{
    auto & kind = cross_cat_.kinds[kindid];
    const ProductModel & partial_model = kind.model;
    auto & partial_mixture = kind.mixture;
    const ProductModel & full_model = algorithm8_.model;
    auto & full_mixture = algorithm8_.kinds[kindid].mixture;

    auto global_groupid = assignments_.groupids(kindid).pop();
    auto groupid = partial_mixture.id_tracker.global_to_packed(global_groupid);
    partial_mixture.remove_value(partial_model, groupid, partial_value, rng);
    full_mixture.remove_value(full_model, groupid, unobserved_, rng);
}

void Loom::algorithm8_work (
        const size_t kindid,
        rng_t::result_type seed)
{
    VectorFloat scores;
    rng_t rng(seed);

    while (auto * envelope = algorithm8_queues_.consumer_receive(kindid)) {

        const Algorithm8Task & task = envelope->message;
        const Value & partial_value = task.partial_values[kindid];
        const Value & full_value = task.full_value;
        if (task.next_action_is_add) {
            algorithm8_work_add(kindid, partial_value, full_value, rng);
        } else {
            algorithm8_work_remove(kindid, partial_value, rng);
        }

        algorithm8_queues_.consumer_free(envelope);
    }
}

inline void Loom::predict_row (
        rng_t & rng,
        const protobuf::PreQL::Predict::Query & query,
        protobuf::PreQL::Predict::Result & result)
{
    result.Clear();
    result.set_id(query.id());
    if (not cross_cat_.schema.is_valid(query.data())) {
        result.set_error("invalid query data");
        return;
    }
    if (query.data().observed_size() != query.to_predict_size()) {
        result.set_error("observed size != to_predict size");
        return;
    }
    const size_t sample_count = query.sample_count();
    if (sample_count == 0) {
        return;
    }

    cross_cat_.value_split(query.data(), partial_values_);
    std::vector<std::vector<Value>> result_factors(1);
    {
        Value sample;
        * sample.mutable_observed() = query.to_predict();
        cross_cat_.value_resize(sample);
        cross_cat_.value_split(sample, result_factors[0]);
        result_factors.resize(sample_count, result_factors[0]);
    }

    const size_t kind_count = cross_cat_.kinds.size();
    for (size_t i = 0; i < kind_count; ++i) {
        if (protobuf::SparseValueSchema::total_size(result_factors[0][i])) {
            const Value & value = partial_values_[i];
            auto & kind = cross_cat_.kinds[i];
            const ProductModel & model = kind.model;
            auto & mixture = kind.mixture;

            mixture.score_value(model, value, scores_, rng);
            float total = distributions::scores_to_likelihoods(scores_);
            distributions::vector_scale(
                scores_.size(),
                scores_.data(),
                1.f / total);
            const VectorFloat & probs = scores_;

            for (auto & result_values : result_factors) {
                mixture.sample_value(model, probs, result_values[i], rng);
            }
        }
    }

    for (const auto & result_values : result_factors) {
        value_join_(* result.add_samples(), result_values);
    }
}

} // namespace loom
