// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2019 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "deconvolutiondepthwise_vulkan.h"
#include <algorithm>
#include "layer_type.h"

namespace ncnn {

DEFINE_LAYER_CREATOR(DeconvolutionDepthWise_vulkan)

DeconvolutionDepthWise_vulkan::DeconvolutionDepthWise_vulkan()
{
    support_vulkan = true;

    crop = 0;
    packing_pack1 = 0;
    packing_pack4 = 0;

    pipeline_deconvolutiondepthwise = 0;
    pipeline_deconvolutiondepthwise_pack4 = 0;

    pipeline_deconvolutiondepthwise_group = 0;
    pipeline_deconvolutiondepthwise_group_pack4 = 0;
    pipeline_deconvolutiondepthwise_group_pack1to4 = 0;
    pipeline_deconvolutiondepthwise_group_pack4to1 = 0;
}

int DeconvolutionDepthWise_vulkan::create_pipeline(const Option& opt)
{
    {
        crop = ncnn::create_layer(ncnn::LayerType::Crop);
        crop->vkdev = vkdev;

        ncnn::ParamDict pd;
        pd.set(0, pad_w);
        pd.set(1, pad_h);
        pd.set(2, 0);

        crop->load_param(pd);

        crop->create_pipeline(opt);
    }

    std::vector<vk_specialization_type> specializations(11);
    specializations[0].i = kernel_w;
    specializations[1].i = kernel_h;
    specializations[2].i = dilation_w;
    specializations[3].i = dilation_h;
    specializations[4].i = stride_w;
    specializations[5].i = stride_h;
    specializations[6].i = bias_term;
    specializations[7].i = group;
    specializations[8].i = activation_type;
    specializations[9].f = activation_params.w == 1 ? activation_params[0] : 0.f;
    specializations[10].f = activation_params.w == 2 ? activation_params[1] : 0.f;

    const int maxk = kernel_w * kernel_h;
    int channels = (weight_data_size / group) / maxk / (num_output / group) * group;

    // depth-wise
    if (channels == group && group == num_output)
    {
        // pack1
        if (num_output % 4 != 0)
        {
            pipeline_deconvolutiondepthwise = new Pipeline(vkdev);
            pipeline_deconvolutiondepthwise->set_optimal_local_size_xyz(32, 32, num_output);
            pipeline_deconvolutiondepthwise->create("deconvolutiondepthwise", opt, specializations, 4, 10);
        }

        // pack4
        if (num_output % 4 == 0)
        {
            pipeline_deconvolutiondepthwise_pack4 = new Pipeline(vkdev);
            pipeline_deconvolutiondepthwise_pack4->set_optimal_local_size_xyz(32, 32, std::max(1, num_output / 4));
            pipeline_deconvolutiondepthwise_pack4->create("deconvolutiondepthwise_pack4", opt, specializations, 4, 10);
        }

        return 0;
    }

    // group deconvolution
    const int channels_g = channels / group;
    const int num_output_g = num_output / group;

    // pack1
    if (channels_g % 4 != 0 && num_output_g % 4 != 0)
    {
        pipeline_deconvolutiondepthwise_group = new Pipeline(vkdev);
        pipeline_deconvolutiondepthwise_group->set_optimal_local_size_xyz(32, 32, std::max(1, num_output / 8));
        pipeline_deconvolutiondepthwise_group->create("deconvolutiondepthwise_group", opt, specializations, 4, 10);
    }

    // pack4
    if (channels_g % 4 == 0 && num_output_g % 4 == 0)
    {
        pipeline_deconvolutiondepthwise_group_pack4 = new Pipeline(vkdev);
        pipeline_deconvolutiondepthwise_group_pack4->set_optimal_local_size_xyz(32, 32, std::max(1, num_output / 8));
        pipeline_deconvolutiondepthwise_group_pack4->create("deconvolutiondepthwise_group_pack4", opt, specializations, 4, 10);
    }

    // pack1to4
    if (channels_g % 4 != 0 && num_output_g % 4 == 0)
    {
        pipeline_deconvolutiondepthwise_group_pack1to4 = new Pipeline(vkdev);
        pipeline_deconvolutiondepthwise_group_pack1to4->set_optimal_local_size_xyz(32, 32, std::max(1, num_output / 8));
        pipeline_deconvolutiondepthwise_group_pack1to4->create("deconvolutiondepthwise_group_pack1to4", opt, specializations, 4, 10);
    }

    // pack4to1
    if (channels_g % 4 == 0 && num_output_g % 4 != 0)
    {
        pipeline_deconvolutiondepthwise_group_pack4to1 = new Pipeline(vkdev);
        pipeline_deconvolutiondepthwise_group_pack4to1->set_optimal_local_size_xyz(32, 32, std::max(1, num_output / 8));
        pipeline_deconvolutiondepthwise_group_pack4to1->create("deconvolutiondepthwise_group_pack4to1", opt, specializations, 4, 10);
    }

    if (channels % 4 == 0 && channels_g % 4 != 0)
    {
        packing_pack1 = ncnn::create_layer(ncnn::LayerType::Packing);
        packing_pack1->vkdev = vkdev;

        ncnn::ParamDict pd;
        pd.set(0, 1);

        packing_pack1->load_param(pd);

        packing_pack1->create_pipeline(opt);
    }

    if (num_output_g % 4 != 0 && num_output % 4 == 0)
    {
        packing_pack4 = ncnn::create_layer(ncnn::LayerType::Packing);
        packing_pack4->vkdev = vkdev;

        ncnn::ParamDict pd;
        pd.set(0, 4);

        packing_pack4->load_param(pd);

        packing_pack4->create_pipeline(opt);
    }

    return 0;
}

int DeconvolutionDepthWise_vulkan::destroy_pipeline(const Option& opt)
{
    if (crop)
    {
        crop->destroy_pipeline(opt);
        delete crop;
        crop = 0;
    }

    if (packing_pack1)
    {
        packing_pack1->destroy_pipeline(opt);
        delete packing_pack1;
        packing_pack1 = 0;
    }

    if (packing_pack4)
    {
        packing_pack4->destroy_pipeline(opt);
        delete packing_pack4;
        packing_pack4 = 0;
    }

    delete pipeline_deconvolutiondepthwise;
    pipeline_deconvolutiondepthwise = 0;

    delete pipeline_deconvolutiondepthwise_pack4;
    pipeline_deconvolutiondepthwise_pack4 = 0;

    delete pipeline_deconvolutiondepthwise_group;
    pipeline_deconvolutiondepthwise_group = 0;

    delete pipeline_deconvolutiondepthwise_group_pack4;
    pipeline_deconvolutiondepthwise_group_pack4 = 0;

    delete pipeline_deconvolutiondepthwise_group_pack1to4;
    pipeline_deconvolutiondepthwise_group_pack1to4 = 0;

    delete pipeline_deconvolutiondepthwise_group_pack4to1;
    pipeline_deconvolutiondepthwise_group_pack4to1 = 0;

    return 0;
}

int DeconvolutionDepthWise_vulkan::upload_model(VkTransfer& cmd, const Option& opt)
{
    const int maxk = kernel_w * kernel_h;
    int channels = (weight_data_size / group) / maxk / (num_output / group) * group;

    Mat weight_data_transposed(weight_data.w);
    {
        float* pt = weight_data_transposed;
        const float* p = weight_data;

        for (int i=0; i<(channels/group)*(num_output/group)*group; i++)
        {
            for (int k=0; k<maxk; k++)
            {
                pt[maxk-1 - k] = p[k];
            }

            p += maxk;
            pt += maxk;
        }
    }

    // depth-wise
    if (channels == group && group == num_output)
    {
        // pack1
        if (num_output % 4 != 0)
        {
            cmd.record_upload(weight_data_transposed, weight_data_gpu, opt);
        }

        // pack4
        if (num_output % 4 == 0)
        {
            const int maxk = kernel_w * kernel_h;

            Mat weight_data_pack4;
            Mat weight_data_r2 = weight_data_transposed.reshape(maxk, group);
            convert_packing(weight_data_r2, weight_data_pack4, 4);

            weight_data_pack4 = weight_data_pack4.reshape(maxk * (group/4));
            cmd.record_upload(weight_data_pack4, weight_data_gpu_pack4, opt);
        }

        if (bias_term)
        {
            if (num_output % 4 != 0)
            {
                cmd.record_upload(bias_data, bias_data_gpu, opt);
            }

            if (num_output % 4 == 0)
            {
                Mat bias_data_pack4;
                convert_packing(bias_data, bias_data_pack4, 4);
                cmd.record_upload(bias_data_pack4, bias_data_gpu_pack4, opt);
            }
        }

        return 0;
    }

    // group deconvolution
    const int channels_g = channels / group;
    const int num_output_g = num_output / group;

    // pack1
    if (channels_g % 4 != 0 && num_output_g % 4 != 0)
    {
        cmd.record_upload(weight_data_transposed, weight_data_gpu, opt);
    }

    // pack4
    if (channels_g % 4 == 0 && num_output_g % 4 == 0)
    {
        // src = kw-kh-inch-outch
        // dst = 4a-4b-kw-kh-inch/4a-outch/4b
        Mat weight_data_pack4_groups;
        {
            Mat weight_data_r2_groups = weight_data_transposed.reshape(maxk, channels_g, num_output_g * group);

            weight_data_pack4_groups.create(maxk, channels_g/4, num_output_g/4 * group, (size_t)4*16, 16);

            for (int g=0; g<group; g++)
            {
                const Mat weight_data_r2 = weight_data_r2_groups.channel_range(num_output_g * g, num_output_g);

                Mat weight_data_pack4 = weight_data_pack4_groups.channel_range(num_output_g/4 * g, num_output_g/4);

                for (int q=0; q+3<num_output_g; q+=4)
                {
                    const Mat k0 = weight_data_r2.channel(q);
                    const Mat k1 = weight_data_r2.channel(q+1);
                    const Mat k2 = weight_data_r2.channel(q+2);
                    const Mat k3 = weight_data_r2.channel(q+3);

                    Mat g0 = weight_data_pack4.channel(q/4);

                    for (int p=0; p+3<channels_g; p+=4)
                    {
                        const float* k00 = k0.row(p);
                        const float* k01 = k0.row(p+1);
                        const float* k02 = k0.row(p+2);
                        const float* k03 = k0.row(p+3);

                        const float* k10 = k1.row(p);
                        const float* k11 = k1.row(p+1);
                        const float* k12 = k1.row(p+2);
                        const float* k13 = k1.row(p+3);

                        const float* k20 = k2.row(p);
                        const float* k21 = k2.row(p+1);
                        const float* k22 = k2.row(p+2);
                        const float* k23 = k2.row(p+3);

                        const float* k30 = k3.row(p);
                        const float* k31 = k3.row(p+1);
                        const float* k32 = k3.row(p+2);
                        const float* k33 = k3.row(p+3);

                        float* g00 = g0.row(p/4);

                        for (int k=0; k<maxk; k++)
                        {
                            g00[0] = k00[k];
                            g00[1] = k01[k];
                            g00[2] = k02[k];
                            g00[3] = k03[k];

                            g00[4] = k10[k];
                            g00[5] = k11[k];
                            g00[6] = k12[k];
                            g00[7] = k13[k];

                            g00[8] = k20[k];
                            g00[9] = k21[k];
                            g00[10] = k22[k];
                            g00[11] = k23[k];

                            g00[12] = k30[k];
                            g00[13] = k31[k];
                            g00[14] = k32[k];
                            g00[15] = k33[k];

                            g00 += 16;
                        }
                    }
                }
            }
        }

        cmd.record_upload(weight_data_pack4_groups, weight_data_gpu_pack4, opt);
    }

    // pack1to4
    if (channels_g % 4 != 0 && num_output_g % 4 == 0)
    {
        // src = kw-kh-inch-outch
        // dst = 4b-kw-kh-inch-outch/4b
        Mat weight_data_pack1to4_groups;
        {
            Mat weight_data_r2_groups = weight_data_transposed.reshape(maxk, channels_g, num_output_g * group);

            weight_data_pack1to4_groups.create(maxk, channels_g, num_output_g/4 * group, (size_t)4*4, 4);

            for (int g=0; g<group; g++)
            {
                const Mat weight_data_r2 = weight_data_r2_groups.channel_range(num_output_g * g, num_output_g);

                Mat weight_data_pack1to4 = weight_data_pack1to4_groups.channel_range(num_output_g/4 * g, num_output_g/4);

                for (int q=0; q+3<num_output_g; q+=4)
                {
                    const Mat k0 = weight_data_r2.channel(q);
                    const Mat k1 = weight_data_r2.channel(q+1);
                    const Mat k2 = weight_data_r2.channel(q+2);
                    const Mat k3 = weight_data_r2.channel(q+3);

                    Mat g0 = weight_data_pack1to4.channel(q/4);

                    for (int p=0; p<channels_g; p++)
                    {
                        const float* k00 = k0.row(p);
                        const float* k10 = k1.row(p);
                        const float* k20 = k2.row(p);
                        const float* k30 = k3.row(p);

                        float* g00 = g0.row(p);

                        for (int k=0; k<maxk; k++)
                        {
                            g00[0] = k00[k];
                            g00[1] = k10[k];
                            g00[2] = k20[k];
                            g00[3] = k30[k];

                            g00 += 4;
                        }
                    }
                }
            }
        }

        cmd.record_upload(weight_data_pack1to4_groups, weight_data_gpu_pack1to4, opt);
    }

    // pack4to1
    if (channels_g % 4 == 0 && num_output_g % 4 != 0)
    {
        // src = kw-kh-inch-outch
        // dst = 4a-kw-kh-inch/4a-outch
        Mat weight_data_pack4to1_groups;
        {
            Mat weight_data_r2_groups = weight_data_transposed.reshape(maxk, channels_g, num_output_g * group);

            weight_data_pack4to1_groups.create(maxk, channels_g/4, num_output_g * group, (size_t)4*4, 4);

            for (int g=0; g<group; g++)
            {
                const Mat weight_data_r2 = weight_data_r2_groups.channel_range(num_output_g * g, num_output_g);

                Mat weight_data_pack4to1 = weight_data_pack4to1_groups.channel_range(num_output_g * g, num_output_g);

                for (int q=0; q<num_output_g; q++)
                {
                    const Mat k0 = weight_data_r2.channel(q);
                    Mat g0 = weight_data_pack4to1.channel(q);

                    for (int p=0; p+3<channels_g; p+=4)
                    {
                        const float* k00 = k0.row(p);
                        const float* k01 = k0.row(p+1);
                        const float* k02 = k0.row(p+2);
                        const float* k03 = k0.row(p+3);

                        float* g00 = g0.row(p/4);

                        for (int k=0; k<maxk; k++)
                        {
                            g00[0] = k00[k];
                            g00[1] = k01[k];
                            g00[2] = k02[k];
                            g00[3] = k03[k];

                            g00 += 4;
                        }
                    }
                }
            }
        }

        cmd.record_upload(weight_data_pack4to1_groups, weight_data_gpu_pack4to1, opt);
    }

    if (bias_term)
    {
        if (num_output_g % 4 != 0)
        {
            cmd.record_upload(bias_data, bias_data_gpu, opt);
        }

        if (num_output_g % 4 == 0)
        {
            Mat bias_data_pack4;
            convert_packing(bias_data, bias_data_pack4, 4);
            cmd.record_upload(bias_data_pack4, bias_data_gpu_pack4, opt);
        }
    }

    return 0;
}

int DeconvolutionDepthWise_vulkan::forward(const VkMat& bottom_blob, VkMat& top_blob, VkCompute& cmd, const Option& opt) const
{
    int w = bottom_blob.w;
    int h = bottom_blob.h;
    int channels = bottom_blob.c;
    size_t elemsize = bottom_blob.elemsize;
    int packing = bottom_blob.packing;

    const int kernel_extent_w = dilation_w * (kernel_w - 1) + 1;
    const int kernel_extent_h = dilation_h * (kernel_h - 1) + 1;

    int outw = (w - 1) * stride_w + kernel_extent_w;
    int outh = (h - 1) * stride_h + kernel_extent_h;
    int out_packing = num_output % 4 == 0 ? 4 : 1;
    size_t out_elemsize = elemsize / packing * out_packing;

    VkMat top_blob_bordered;
    if (pad_w > 0 || pad_h > 0)
    {
        top_blob_bordered.create(outw, outh, num_output / out_packing, out_elemsize, out_packing, opt.workspace_vkallocator, opt.staging_vkallocator);
        if (top_blob_bordered.empty())
            return -100;
    }
    else
    {
        top_blob_bordered.create(outw, outh, num_output / out_packing, out_elemsize, out_packing, opt.blob_vkallocator, opt.staging_vkallocator);
        if (top_blob_bordered.empty())
            return -100;
    }

    // depth-wise
    if (channels == group / packing && group / packing == num_output / packing)
    {
        std::vector<VkMat> bindings(4);
        bindings[0] = bottom_blob;
        bindings[1] = top_blob_bordered;
        bindings[2] = packing == 4 ? weight_data_gpu_pack4 : weight_data_gpu;
        bindings[3] = bias_term ? (packing == 4 ? bias_data_gpu_pack4 : bias_data_gpu) : bindings[2];// TODO use dummy buffer

        std::vector<vk_constant_type> constants(10);
        constants[0].i = bottom_blob.dims;
        constants[1].i = bottom_blob.w;
        constants[2].i = bottom_blob.h;
        constants[3].i = bottom_blob.c;
        constants[4].i = bottom_blob.cstep;
        constants[5].i = top_blob_bordered.dims;
        constants[6].i = top_blob_bordered.w;
        constants[7].i = top_blob_bordered.h;
        constants[8].i = top_blob_bordered.c;
        constants[9].i = top_blob_bordered.cstep;

        const Pipeline* pipeline = packing == 4 ? pipeline_deconvolutiondepthwise_pack4 : pipeline_deconvolutiondepthwise;

        // record
        cmd.record_pipeline(pipeline, bindings, constants, top_blob_bordered);

        if (pad_w > 0 || pad_h > 0)
        {
            VkMat reference_blob;
            reference_blob.dims = 2;
            reference_blob.w = top_blob_bordered.w - pad_w - pad_w;
            reference_blob.h = top_blob_bordered.h - pad_h - pad_h;

            std::vector<VkMat> crop_bottom_blobs(2);
            crop_bottom_blobs[0] = top_blob_bordered;
            crop_bottom_blobs[1] = reference_blob;
            std::vector<VkMat> crop_top_blobs(1);
            crop->forward(crop_bottom_blobs, crop_top_blobs, cmd, opt);
            top_blob = crop_top_blobs[0];

            outw = top_blob.w;
            outh = top_blob.h;
        }
        else
        {
            top_blob = top_blob_bordered;
        }

        return 0;
    }

    const int channels_g = channels * packing / group;
    const int num_output_g = num_output / group;

    // unpacking
    VkMat bottom_blob_unpacked = bottom_blob;
    if (packing == 4 && channels_g % 4 != 0)
    {
        ncnn::Option opt_pack1 = opt;
        opt_pack1.blob_vkallocator = opt.workspace_vkallocator;

        packing_pack1->forward(bottom_blob, bottom_blob_unpacked, cmd, opt_pack1);
    }

    VkMat top_blob_unpacked = top_blob_bordered;
    if (num_output_g % 4 != 0 && out_packing == 4)
    {
        top_blob_unpacked.create(outw, outh, num_output, elemsize / packing, 1, opt.workspace_vkallocator, opt.staging_vkallocator);
        if (top_blob_unpacked.empty())
            return -100;
    }

    std::vector<VkMat> bindings(4);
    bindings[0] = bottom_blob_unpacked;
    bindings[1] = top_blob_unpacked;
    if (channels_g % 4 != 0 && num_output_g % 4 != 0)
    {
        bindings[2] = weight_data_gpu;
        bindings[3] = bias_term ? bias_data_gpu : bindings[2];// TODO use dummy buffer
    }
    else if (channels_g % 4 == 0 && num_output_g % 4 == 0)
    {
        bindings[2] = weight_data_gpu_pack4;
        bindings[3] = bias_term ? bias_data_gpu_pack4 : bindings[2];// TODO use dummy buffer
    }
    else if (channels_g % 4 != 0 && num_output_g % 4 == 0)
    {
        bindings[2] = weight_data_gpu_pack1to4;
        bindings[3] = bias_term ? bias_data_gpu_pack4 : bindings[2];// TODO use dummy buffer
    }
    else if (channels_g % 4 == 0 && num_output_g % 4 != 0)
    {
        bindings[2] = weight_data_gpu_pack4to1;
        bindings[3] = bias_term ? bias_data_gpu : bindings[2];// TODO use dummy buffer
    }

    std::vector<vk_constant_type> constants(10);
    constants[0].i = bottom_blob_unpacked.dims;
    constants[1].i = bottom_blob_unpacked.w;
    constants[2].i = bottom_blob_unpacked.h;
    constants[3].i = bottom_blob_unpacked.c;
    constants[4].i = bottom_blob_unpacked.cstep;
    constants[5].i = top_blob_unpacked.dims;
    constants[6].i = top_blob_unpacked.w;
    constants[7].i = top_blob_unpacked.h;
    constants[8].i = top_blob_unpacked.c;
    constants[9].i = top_blob_unpacked.cstep;

    const Pipeline* pipeline = 0;
    if (channels_g % 4 != 0 && num_output_g % 4 != 0)
    {
        pipeline = pipeline_deconvolutiondepthwise_group;
    }
    else if (channels_g % 4 == 0 && num_output_g % 4 == 0)
    {
        pipeline = pipeline_deconvolutiondepthwise_group_pack4;
    }
    else if (channels_g % 4 != 0 && num_output_g % 4 == 0)
    {
        pipeline = pipeline_deconvolutiondepthwise_group_pack1to4;
    }
    else if (channels_g % 4 == 0 && num_output_g % 4 != 0)
    {
        pipeline = pipeline_deconvolutiondepthwise_group_pack4to1;
    }

    cmd.record_pipeline(pipeline, bindings, constants, top_blob_unpacked);

    // packing
    if (num_output_g % 4 != 0 && out_packing == 4)
    {
        packing_pack4->forward(top_blob_unpacked, top_blob_bordered, cmd, opt);
    }
    else
    {
        top_blob_bordered = top_blob_unpacked;
    }

    if (pad_w > 0 || pad_h > 0)
    {
        VkMat reference_blob;
        reference_blob.dims = 2;
        reference_blob.w = top_blob_bordered.w - pad_w - pad_w;
        reference_blob.h = top_blob_bordered.h - pad_h - pad_h;

        std::vector<VkMat> crop_bottom_blobs(2);
        crop_bottom_blobs[0] = top_blob_bordered;
        crop_bottom_blobs[1] = reference_blob;
        std::vector<VkMat> crop_top_blobs(1);
        crop->forward(crop_bottom_blobs, crop_top_blobs, cmd, opt);
        top_blob = crop_top_blobs[0];

        outw = top_blob.w;
        outh = top_blob.h;
    }
    else
    {
        top_blob = top_blob_bordered;
    }

    return 0;
}

} // namespace ncnn
