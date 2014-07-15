// Copyright (c) 2014, Salesforce.com, Inc.  All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// - Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
// - Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
// - Neither the name of Salesforce.com nor the names of its contributors
//   may be used to endorse or promote products derived from this
//   software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
// COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
// OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
// TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
// USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <loom/product_value.hpp>

namespace loom
{

void ValueSplitter::init (
        const ValueSchema & schema,
        const std::vector<uint32_t> & full_to_partid,
        size_t part_count)
{
    const size_t feature_count = schema.total_size();
    LOOM_ASSERT_EQ(full_to_partid.size(), feature_count);
    if (LOOM_DEBUG_LEVEL >= 2) {
        for (auto partid : full_to_partid) {
            LOOM_ASSERT_LT(partid, part_count);
        }
    }

    this->schema = schema;
    this->full_to_partid = full_to_partid;
    part_schemas.clear();
    part_schemas.resize(part_count);
    full_to_part.resize(feature_count);

    size_t full_pos = 0;
    size_t end;
    for (end = full_pos + schema.booleans_size; full_pos < end; ++full_pos) {
        auto & part_schema = part_schemas[full_to_partid[full_pos]];
        full_to_part[full_pos] = part_schema.total_size();
        part_schema.booleans_size += 1;
    }
    for (end = full_pos + schema.counts_size; full_pos < end; ++full_pos) {
        auto & part_schema = part_schemas[full_to_partid[full_pos]];
        full_to_part[full_pos] = part_schema.total_size();
        part_schema.counts_size += 1;
    }
    for (end = full_pos + schema.reals_size; full_pos < end; ++full_pos) {
        auto & part_schema = part_schemas[full_to_partid[full_pos]];
        full_to_part[full_pos] = part_schema.total_size();
        part_schema.reals_size += 1;
    }
    LOOM_ASSERT_EQ(full_pos, feature_count);
}

struct ValueSplitter::split_value_all_fun
{
    const std::vector<uint32_t> & full_to_partid;
    const ProductValue & full_value;
    std::vector<ProductValue> & partial_values;
    size_t full_pos;

    template<class FieldType>
    void operator() (FieldType *, size_t size)
    {
        typedef protobuf::Fields<FieldType> Fields;
        auto full_fields = Fields::get(full_value).begin();
        for (size_t end = full_pos + size; full_pos < end; ++full_pos) {
            auto & partial_value = partial_values[full_to_partid[full_pos]];
            Fields::get(partial_value).Add(*full_fields++);
        }
        LOOM_ASSERT1(
            full_fields == Fields::get(full_value).end(),
            "programmer error");
    }
};

struct ValueSplitter::split_value_dense_fun
{
    const std::vector<uint32_t> & full_to_partid;
    const ProductValue & full_value;
    std::vector<ProductValue> & partial_values;
    size_t full_pos;

    template<class FieldType>
    void operator() (FieldType *, size_t size)
    {
        typedef protobuf::Fields<FieldType> Fields;
        auto full_fields = Fields::get(full_value).begin();
        for (size_t end = full_pos + size; full_pos < end; ++full_pos) {
            auto & partial_value = partial_values[full_to_partid[full_pos]];
            bool observed = full_value.observed().dense(full_pos);
            partial_value.mutable_observed()->add_dense(observed);
            if (observed) {
                Fields::get(partial_value).Add(*full_fields++);
            }
        }
        LOOM_ASSERT1(
            full_fields == Fields::get(full_value).end(),
            "programmer error");
    }
};

struct ValueSplitter::split_value_sparse_fun
{
    const std::vector<uint32_t> & full_to_partid;
    const std::vector<uint32_t> & full_to_part;
    const ProductValue & full_value;
    std::vector<ProductValue> & partial_values;
    decltype(full_value.observed().sparse().begin()) it;
    decltype(full_value.observed().sparse().begin()) end;
    BlockIterator block;

    template<class FieldType>
    void operator() (FieldType *, size_t size)
    {
        typedef protobuf::Fields<FieldType> Fields;
        auto full_fields = Fields::get(full_value).begin();
        for (block(size); it != end and block.ok(*it); ++it) {
            auto full_pos = *it;
            auto & partial_value = partial_values[full_to_partid[full_pos]];
            auto part_pos = full_to_part[full_pos];
            partial_value.mutable_observed()->add_sparse(part_pos);
            Fields::get(partial_value).Add(*full_fields++);
        }
        LOOM_ASSERT1(
            full_fields == Fields::get(full_value).end(),
            "programmer error");
    }
};

void ValueSplitter::split (
        const ProductValue & full_value,
        std::vector<ProductValue> & partial_values) const
{
    try {
        validate(full_value);

        partial_values.resize(part_schemas.size());
        auto sparsity = full_value.observed().sparsity();
        for (auto & partial_value : partial_values) {
            partial_value.Clear();
            partial_value.mutable_observed()->set_sparsity(sparsity);
        }

        switch (sparsity) {
            case ProductValue::Observed::ALL: {
                split_value_all_fun fun = {
                    full_to_partid,
                    full_value,
                    partial_values,
                    0};
                schema.for_each_datatype(fun);
                LOOM_ASSERT1(
                    fun.full_pos == full_to_partid.size(),
                    "programmer error");
            } break;

            case ProductValue::Observed::DENSE: {
                split_value_dense_fun fun = {
                    full_to_partid,
                    full_value,
                    partial_values,
                    0};
                schema.for_each_datatype(fun);
                LOOM_ASSERT1(
                    fun.full_pos == full_to_partid.size(),
                    "programmer error");
            } break;

            case ProductValue::Observed::SPARSE: {
                split_value_sparse_fun fun = {
                    full_to_partid,
                    full_to_part,
                    full_value,
                    partial_values,
                    full_value.observed().sparse().begin(),
                    full_value.observed().sparse().end(),
                    BlockIterator()};
                schema.for_each_datatype(fun);
                LOOM_ASSERT1(fun.it == fun.end, "programmer error");
            } break;

            case ProductValue::Observed::NONE:
                break;
        }

        validate(partial_values);
    } catch (google::protobuf::FatalException e) {
        LOOM_ERROR(e.what());
    }
}

struct ValueSplitter::split_observed_dense_fun
{
    const ValueSplitter & splitter;
    const ProductValue::Observed & full_observed;
    std::vector<ProductValue> & partial_values;
    size_t full_pos;

    template<class FieldType>
    void operator() (FieldType *, size_t size)
    {
        for (size_t end = full_pos + size; full_pos < end; ++full_pos) {
            auto partid = splitter.full_to_partid[full_pos];
            auto & partial_value = partial_values[partid];
            bool observed = full_observed.dense(full_pos);
            partial_value.mutable_observed()->add_dense(observed);
        }
    }
};

void ValueSplitter::split_observed (
        const ProductValue::Observed & full_observed,
        std::vector<ProductValue> & partial_values) const
{
    try {
        LOOM_ASSERT_EQ(full_observed.sparsity(), ProductValue::Observed::DENSE);
        LOOM_ASSERT_EQ(full_observed.dense_size(), schema.total_size());
        LOOM_ASSERT_EQ(full_observed.sparse_size(), 0);

        partial_values.resize(part_schemas.size());
        for (auto & partial_value : partial_values) {
            partial_value.Clear();
            partial_value.mutable_observed()->set_sparsity(
                ProductValue::Observed::DENSE);
        }

        split_observed_dense_fun fun = {
            *this,
            full_observed,
            partial_values,
            0};
        schema.for_each_datatype(fun);
        LOOM_ASSERT1(fun.full_pos == full_to_partid.size(), "programmer error");
    } catch (google::protobuf::FatalException e) {
        LOOM_ERROR(e.what());
    }
}

struct ValueSplitter::join_value_dense_fun
{
    const ValueSplitter & splitter;
    ProductValue & full_value;
    const std::vector<ProductValue> & partial_values;
    size_t full_pos;

    template<class FieldType>
    void operator() (FieldType *, size_t size)
    {
        if (size) {
            auto & absolute_pos_list = splitter.absolute_pos_list_;
            auto & packed_pos_list = splitter.packed_pos_list_;
            typedef protobuf::Fields<FieldType> Fields;
            auto & full_fields = Fields::get(full_value);
            std::fill(packed_pos_list.begin(), packed_pos_list.end(), 0);
            for (size_t end = full_pos + size; full_pos < end; ++full_pos) {
                auto partid = splitter.full_to_partid[full_pos];
                auto & partial_value = partial_values[partid];
                auto & absolute_pos = absolute_pos_list[partid];
                bool observed = partial_value.observed().dense(absolute_pos++);
                full_value.mutable_observed()->add_dense(observed);
                if (observed) {
                    auto & packed_pos = packed_pos_list[partid];
                    auto & partial_fields = Fields::get(partial_value);
                    full_fields.Add(partial_fields.Get(packed_pos++));
                }
            }
        }
    }
};

void ValueSplitter::join (
        ProductValue & full_value,
        const std::vector<ProductValue> & partial_values) const
{
    try {
        validate(partial_values);
        auto sparsity = partial_values[0].observed().sparsity();
        LOOM_ASSERT_EQ(sparsity, ProductValue::Observed::DENSE);
        const size_t part_count = part_schemas.size();

        full_value.Clear();
        full_value.mutable_observed()->set_sparsity(sparsity);
        absolute_pos_list_.clear();
        absolute_pos_list_.resize(part_count, 0);
        packed_pos_list_.resize(part_count);
        join_value_dense_fun fun = {*this, full_value, partial_values, 0};
        schema.for_each_datatype(fun);

        if (LOOM_DEBUG_LEVEL >= 1) {
            LOOM_ASSERT_EQ(fun.full_pos, full_to_partid.size());
        }

        validate(full_value);
    } catch (google::protobuf::FatalException e) {
        LOOM_ERROR(e.what());
    }
}

} // namespace loom