// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License

#include <memory>

#include <faiss/gpu/GpuCloner.h>
#include <faiss/gpu/GpuIndexIVF.h>
#include <faiss/gpu/GpuIndexIVFFlat.h>
#include <faiss/index_io.h>
#include <fiu-local.h>
#include <string>

#include "knowhere/common/Exception.h"
#include "knowhere/index/vector_index/adapter/VectorAdapter.h"
#include "knowhere/index/vector_index/helpers/Cloner.h"
#include "knowhere/index/vector_index/helpers/FaissIO.h"
#include "knowhere/index/vector_index/helpers/IndexParameter.h"
#include "knowhere/index/vector_offset_index/IndexIVF_NM.h"
#include "knowhere/index/vector_offset_index/gpu/IndexGPUIVF_NM.h"

namespace milvus {
namespace knowhere {

void
GPUIVF_NM::Train(const DatasetPtr& dataset_ptr, const Config& config) {
    GETTENSOR(dataset_ptr)
    gpu_id_ = config[knowhere::meta::DEVICEID];

    auto gpu_res = FaissGpuResourceMgr::GetInstance().GetRes(gpu_id_);
    if (gpu_res != nullptr) {
        ResScope rs(gpu_res, gpu_id_, true);
        faiss::gpu::GpuIndexIVFFlatConfig idx_config;
        idx_config.device = gpu_id_;
        int32_t nlist = config[IndexParams::nlist];
        faiss::MetricType metric_type = GetMetricType(config[Metric::TYPE].get<std::string>());
        faiss::gpu::GpuIndexIVFFlat device_index(gpu_res->faiss_res.get(), dim, nlist, metric_type, idx_config);
        device_index.train(rows, (float*)p_data);

        std::shared_ptr<faiss::Index> host_index = nullptr;
        host_index.reset(faiss::gpu::index_gpu_to_cpu(&device_index));

        auto device_index1 = faiss::gpu::index_cpu_to_gpu(gpu_res->faiss_res.get(), gpu_id_, host_index.get());
        index_.reset(device_index1);
        res_ = gpu_res;
    } else {
        KNOWHERE_THROW_MSG("Build IVF can't get gpu resource");
    }
}

void
GPUIVF_NM::Add(const DatasetPtr& dataset_ptr, const Config& config) {
    if (auto spt = res_.lock()) {
        ResScope rs(res_, gpu_id_);
        IVF::Add(dataset_ptr, config);
    } else {
        KNOWHERE_THROW_MSG("Add IVF can't get gpu resource");
    }
}

void
GPUIVF_NM::Load(const BinarySet& binary_set) {
    /*
    std::lock_guard<std::mutex> lk(mutex_);
    auto binary = binary_set.GetByName("IVF");
    MemoryIOReader reader;
    reader.total = binary->size;
    reader.data_ = binary->data.get();
    faiss::Index* index = faiss::read_index_nm(&reader);
    index_.reset(index);
    // Construct arranged data from original data
    auto binary_data = binary_set.GetByName(RAW_DATA);
    const float* original_data = (const float*) binary_data->data.get();
    auto ivf_index = dynamic_cast<faiss::IndexIVF*>(index_.get());
    auto invlists = ivf_index->invlists;
    auto ails = dynamic_cast<faiss::ArrayInvertedLists*>(invlists);
    auto d = ivf_index->d;
    auto nb = (size_t) (binary_data->size / ails->code_size);
    arranged_data = new uint8_t[d * sizeof(float) * nb];
    size_t curr_index = 0;
    for (int i = 0; i < ails->nlist; i++) {
        auto list_size = ails->ids[i].size();
        for (int j = 0; j < list_size; j++) {
            memcpy(arranged_data + d * sizeof(float) * (curr_index + j), original_data + d * ails->ids[i][j],
                   d * sizeof(float));
        }
        curr_index += list_size;
    }
    if (auto temp_res = FaissGpuResourceMgr::GetInstance().GetRes(gpu_id_)) {
        ResScope rs(temp_res, gpu_id_, false);
        auto device_index =
            faiss::gpu::index_cpu_to_gpu_without_codes(temp_res->faiss_res.get(), gpu_id_, index, arranged_data);
        index_.reset(device_index);
        res_ = temp_res;
    } else {
        KNOWHERE_THROW_MSG("Load error, can't get gpu resource");
    }
    delete index;
    */

    // not supported
}

VecIndexPtr
GPUIVF_NM::CopyGpuToCpu(const Config& config) {
    std::lock_guard<std::mutex> lk(mutex_);

    if (auto device_idx = std::dynamic_pointer_cast<faiss::gpu::GpuIndexIVF>(index_)) {
        faiss::Index* device_index = index_.get();
        faiss::Index* host_index = faiss::gpu::index_gpu_to_cpu_without_codes(device_index);

        std::shared_ptr<faiss::Index> new_index;
        new_index.reset(host_index);
        return std::make_shared<IVF_NM>(new_index);
    } else {
        return std::make_shared<IVF_NM>(index_);
    }
}

VecIndexPtr
GPUIVF_NM::CopyGpuToGpu(const int64_t device_id, const Config& config) {
    auto host_index = CopyGpuToCpu(config);
    return std::static_pointer_cast<IVF>(host_index)->CopyCpuToGpu(device_id, config);
}

BinarySet
GPUIVF_NM::SerializeImpl(const IndexType& type) {
    if (!index_ || !index_->is_trained) {
        KNOWHERE_THROW_MSG("index not initialize or trained");
    }

    try {
        fiu_do_on("GPUIVF_NM.SerializeImpl.throw_exception", throw std::exception());
        MemoryIOWriter writer;
        {
            faiss::Index* index = index_.get();
            faiss::Index* host_index = faiss::gpu::index_gpu_to_cpu_without_codes(index);
            faiss::write_index_nm(host_index, &writer);
            delete host_index;
        }
        std::shared_ptr<uint8_t[]> data(writer.data_);

        BinarySet res_set;
        res_set.Append("IVF", data, writer.rp);

        return res_set;
    } catch (std::exception& e) {
        KNOWHERE_THROW_MSG(e.what());
    }
}

void
GPUIVF_NM::QueryImpl(int64_t n, const float* data, int64_t k, float* distances, int64_t* labels, const Config& config) {
    std::lock_guard<std::mutex> lk(mutex_);

    auto device_index = std::dynamic_pointer_cast<faiss::gpu::GpuIndexIVF>(index_);
    fiu_do_on("GPUIVF_NM.search_impl.invald_index", device_index = nullptr);
    if (device_index) {
        device_index->nprobe = config[IndexParams::nprobe];
        ResScope rs(res_, gpu_id_);

        // if query size > 2048 we search by blocks to avoid malloc issue
        const int64_t block_size = 2048;
        int64_t dim = device_index->d;
        for (int64_t i = 0; i < n; i += block_size) {
            int64_t search_size = (n - i > block_size) ? block_size : (n - i);
            device_index->search(search_size, (float*)data + i * dim, k, distances + i * k, labels + i * k, bitset_);
        }
    } else {
        KNOWHERE_THROW_MSG("Not a GpuIndexIVF type.");
    }
}

}  // namespace knowhere
}  // namespace milvus
