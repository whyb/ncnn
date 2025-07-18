// Copyright 2021 Tencent
// SPDX-License-Identifier: BSD-3-Clause

#include "crop_riscv.h"

#if __riscv_vector
#include <riscv_vector.h>
#endif // __riscv_vector

#include "riscv_usability.h"

#include "cpu.h"

namespace ncnn {

Crop_riscv::Crop_riscv()
{
#if __riscv_vector
    support_packing = true;
#endif // __riscv_vector
#if NCNN_ZFH
#if __riscv_vector
    support_fp16_storage = cpu_support_riscv_zvfh();
#else
    support_fp16_storage = cpu_support_riscv_zfh();
#endif
#endif

#if NCNN_BF16
    support_bf16_storage = true;
#endif
}

#if __riscv_vector
static void crop_packn_rvv(const Mat& src, Mat& dst, int top, int left, int packn)
{
    int w = dst.w;
    int h = dst.h;
    int right = src.w - dst.w - left;

    const size_t vl = __riscv_vsetvl_e32m1(packn);

    const float* ptr = src.row(top) + left * packn;
    float* outptr = dst;

    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            vfloat32m1_t _p = __riscv_vle32_v_f32m1(ptr, vl);
            __riscv_vse32_v_f32m1(outptr, _p, vl);

            ptr += packn;
            outptr += packn;
        }

        ptr += (left + right) * packn;
    }
}

static void crop_packn_bf16_fp16s_rvv(const Mat& src, Mat& dst, int top, int left, int packn)
{
    int w = dst.w;
    int h = dst.h;
    int right = src.w - dst.w - left;

    const size_t vl = __riscv_vsetvl_e16m1(packn);

    const unsigned short* ptr = src.row<unsigned short>(top) + left * packn;
    unsigned short* outptr = dst;

    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            vuint16m1_t _p = __riscv_vle16_v_u16m1(ptr, vl);
            __riscv_vse16_v_u16m1(outptr, _p, vl);

            ptr += packn;
            outptr += packn;
        }

        ptr += (left + right) * packn;
    }
}
#endif // __riscv_vector

int Crop_riscv::forward(const Mat& bottom_blob, Mat& top_blob, const Option& opt) const
{
    int elembits = bottom_blob.elembits();

#if __riscv_vector
    const int packn = csrr_vlenb() / (elembits / 8);
#endif

    int w = bottom_blob.w;
    int h = bottom_blob.h;
    int d = bottom_blob.d;
    int channels = bottom_blob.c;
    int dims = bottom_blob.dims;
    size_t elemsize = bottom_blob.elemsize;
    int elempack = bottom_blob.elempack;

#if __riscv_vector
    int _woffset, _hoffset, _doffset, _coffset;
    int _outw, _outh, _outd, _outc;
    if (!starts_expr.empty() && !ends_expr.empty())
    {
        std::vector<Mat> bottom_blob_shapes(1);
        bottom_blob_shapes[0] = bottom_blob.shape();
        eval_crop_expr(bottom_blob_shapes, _woffset, _hoffset, _doffset, _coffset, _outw, _outh, _outd, _outc);
    }
    else
    {
        resolve_crop_roi(bottom_blob.shape(), _woffset, _hoffset, _doffset, _coffset, _outw, _outh, _outd, _outc);
    }

    if (elempack == packn)
    {
        if (dims == 1)
        {
            int out_elempack = _outw % packn == 0 ? packn : 1;
            size_t out_elemsize = elemsize / elempack * out_elempack;

            if (_outw / out_elempack == w && out_elempack == packn)
            {
                top_blob = bottom_blob;
                return 0;
            }

            if (_woffset % packn == 0 && out_elempack == packn)
            {
                top_blob.create(_outw / out_elempack, out_elemsize, out_elempack, opt.blob_allocator);
                if (top_blob.empty())
                    return -100;

                if (elembits == 16)
                    crop_packn_bf16_fp16s_rvv(bottom_blob, top_blob, 0, _woffset / elempack, packn);
                else
                    crop_packn_rvv(bottom_blob, top_blob, 0, _woffset / elempack, packn);

                return 0;
            }
        }

        if (dims == 2)
        {
            int out_elempack = _outh % packn == 0 ? packn : 1;
            size_t out_elemsize = elemsize / elempack * out_elempack;

            if (_outw == w && _outh / out_elempack == h && out_elempack == packn)
            {
                top_blob = bottom_blob;
                return 0;
            }

            if (_hoffset % packn == 0 && out_elempack == packn)
            {
                top_blob.create(_outw, _outh / out_elempack, out_elemsize, out_elempack, opt.blob_allocator);
                if (top_blob.empty())
                    return -100;

                if (elembits == 16)
                    crop_packn_bf16_fp16s_rvv(bottom_blob, top_blob, _hoffset / elempack, _woffset, packn);
                else
                    crop_packn_rvv(bottom_blob, top_blob, _hoffset / elempack, _woffset, packn);

                return 0;
            }
        }

        if (dims == 3)
        {
            int out_elempack = _outc % packn == 0 ? packn : 1;
            size_t out_elemsize = elemsize / elempack * out_elempack;

            if (_outw == w && _outh == h && _outc / out_elempack == channels && out_elempack == packn)
            {
                top_blob = bottom_blob;
                return 0;
            }

            if (_coffset % packn == 0 && out_elempack == packn)
            {
                const Mat bottom_blob_sliced = bottom_blob.channel_range(_coffset / out_elempack, _outc / out_elempack);

                if (_outw == w && _outh == h)
                {
                    top_blob = bottom_blob_sliced.clone(opt.blob_allocator);
                    if (top_blob.empty())
                        return -100;
                }

                top_blob.create(_outw, _outh, _outc / out_elempack, out_elemsize, out_elempack, opt.blob_allocator);
                if (top_blob.empty())
                    return -100;

                #pragma omp parallel for num_threads(opt.num_threads)
                for (int q = 0; q < top_blob.c; q++)
                {
                    const Mat m = bottom_blob_sliced.channel(q);
                    Mat borderm = top_blob.channel(q);

                    if (elembits == 16)
                        crop_packn_bf16_fp16s_rvv(m, borderm, _hoffset, _woffset, packn);
                    else
                        crop_packn_rvv(m, borderm, _hoffset, _woffset, packn);
                }

                return 0;
            }
        }

        if (dims == 4)
        {
            int out_elempack = _outc % packn == 0 ? packn : 1;
            size_t out_elemsize = elemsize / elempack * out_elempack;

            if (_outw == w && _outh == h && _outd == d && _outc / out_elempack == channels && out_elempack == packn)
            {
                top_blob = bottom_blob;
                return 0;
            }

            if (_coffset % packn == 0 && out_elempack == packn)
            {
                const Mat bottom_blob_sliced = bottom_blob.channel_range(_coffset / out_elempack, _outc / out_elempack);

                if (_outw == w && _outh == h && _outd == d)
                {
                    top_blob = bottom_blob_sliced.clone(opt.blob_allocator);
                    if (top_blob.empty())
                        return -100;
                }

                top_blob.create(_outw, _outh, _outd, _outc / out_elempack, out_elemsize, out_elempack, opt.blob_allocator);
                if (top_blob.empty())
                    return -100;

                #pragma omp parallel for num_threads(opt.num_threads)
                for (int q = 0; q < top_blob.c; q++)
                {
                    for (int z = 0; z < _outd; z++)
                    {
                        const Mat m = bottom_blob_sliced.channel(q).depth(z + _doffset);
                        Mat borderm = top_blob.channel(q).depth(z);

                        if (elembits == 16)
                            crop_packn_bf16_fp16s_rvv(m, borderm, _hoffset, _woffset, packn);
                        else
                            crop_packn_rvv(m, borderm, _hoffset, _woffset, packn);
                    }
                }

                return 0;
            }
        }
    }
#endif // __riscv_vector

    Mat bottom_blob_unpacked = bottom_blob;
    if (elempack != 1)
    {
        Option opt_pack1 = opt;
        opt_pack1.blob_allocator = opt.workspace_allocator;

        convert_packing(bottom_blob, bottom_blob_unpacked, 1, opt_pack1);
        if (bottom_blob_unpacked.empty())
            return -100;
    }

    return Crop::forward(bottom_blob_unpacked, top_blob, opt);
}

int Crop_riscv::forward(const std::vector<Mat>& bottom_blobs, std::vector<Mat>& top_blobs, const Option& opt) const
{
    const Mat& bottom_blob = bottom_blobs[0];
    const Mat& reference_blob = bottom_blobs[1];

    int elembits = bottom_blob.elembits();

#if __riscv_vector
    const int packn = csrr_vlenb() / (elembits / 8);
#endif

    int w = bottom_blob.w;
    int h = bottom_blob.h;
    int d = bottom_blob.d;
    int channels = bottom_blob.c;
    int dims = bottom_blob.dims;
    size_t elemsize = bottom_blob.elemsize;
    int elempack = bottom_blob.elempack;

    int ref_elempack = reference_blob.elempack;

    Mat& top_blob = top_blobs[0];

#if __riscv_vector
    int _woffset, _hoffset, _doffset, _coffset;
    int _outw, _outh, _outd, _outc;
    if (!starts_expr.empty() && !ends_expr.empty())
    {
        std::vector<Mat> bottom_blob_shapes(bottom_blobs.size());
        for (size_t i = 0; i < bottom_blobs.size(); i++)
        {
            bottom_blob_shapes[i] = bottom_blobs[i].shape();
        }
        eval_crop_expr(bottom_blob_shapes, _woffset, _hoffset, _doffset, _coffset, _outw, _outh, _outd, _outc);
    }
    else if (woffset == -233)
    {
        resolve_crop_roi(bottom_blob.shape(), (const int*)reference_blob, _woffset, _hoffset, _doffset, _coffset, _outw, _outh, _outd, _outc);
    }
    else
    {
        resolve_crop_roi(bottom_blob.shape(), reference_blob.shape(), _woffset, _hoffset, _doffset, _coffset, _outw, _outh, _outd, _outc);
    }

    if (elempack == packn)
    {
        if (dims == 1)
        {
            int out_elempack = _outw % packn == 0 ? packn : 1;
            size_t out_elemsize = elemsize / elempack * out_elempack;

            if (_outw / out_elempack == w && out_elempack == packn)
            {
                top_blob = bottom_blob;
                return 0;
            }

            if (_woffset % packn == 0 && out_elempack == packn)
            {
                top_blob.create(_outw / out_elempack, out_elemsize, out_elempack, opt.blob_allocator);
                if (top_blob.empty())
                    return -100;

                if (elembits == 16)
                    crop_packn_bf16_fp16s_rvv(bottom_blob, top_blob, 0, _woffset / elempack, packn);
                else
                    crop_packn_rvv(bottom_blob, top_blob, 0, _woffset / elempack, packn);

                return 0;
            }
        }

        if (dims == 2)
        {
            int out_elempack = _outh % packn == 0 ? packn : 1;
            size_t out_elemsize = elemsize / elempack * out_elempack;

            if (_outw == w && _outh / out_elempack == h && out_elempack == packn)
            {
                top_blob = bottom_blob;
                return 0;
            }

            if (_hoffset % packn == 0 && out_elempack == packn)
            {
                top_blob.create(_outw, _outh / out_elempack, out_elemsize, out_elempack, opt.blob_allocator);
                if (top_blob.empty())
                    return -100;

                if (elembits == 16)
                    crop_packn_bf16_fp16s_rvv(bottom_blob, top_blob, _hoffset / elempack, _woffset, packn);
                else
                    crop_packn_rvv(bottom_blob, top_blob, _hoffset / elempack, _woffset, packn);

                return 0;
            }
        }

        if (dims == 3)
        {
            int out_elempack = _outc % packn == 0 ? packn : 1;
            size_t out_elemsize = elemsize / elempack * out_elempack;

            if (_outw == w && _outh == h && _outc / out_elempack == channels && out_elempack == packn)
            {
                top_blob = bottom_blob;
                return 0;
            }

            if (_coffset % packn == 0 && out_elempack == packn)
            {
                const Mat bottom_blob_sliced = bottom_blob.channel_range(_coffset / out_elempack, _outc / out_elempack);

                if (_outw == w && _outh == h)
                {
                    top_blob = bottom_blob_sliced.clone(opt.blob_allocator);
                    if (top_blob.empty())
                        return -100;
                }

                top_blob.create(_outw, _outh, _outc / out_elempack, out_elemsize, out_elempack, opt.blob_allocator);
                if (top_blob.empty())
                    return -100;

                #pragma omp parallel for num_threads(opt.num_threads)
                for (int q = 0; q < top_blob.c; q++)
                {
                    const Mat m = bottom_blob_sliced.channel(q);
                    Mat borderm = top_blob.channel(q);

                    if (elembits == 16)
                        crop_packn_bf16_fp16s_rvv(m, borderm, _hoffset, _woffset, packn);
                    else
                        crop_packn_rvv(m, borderm, _hoffset, _woffset, packn);
                }

                return 0;
            }
        }

        if (dims == 4)
        {
            int out_elempack = _outc % packn == 0 ? packn : 1;
            size_t out_elemsize = elemsize / elempack * out_elempack;

            if (_outw == w && _outh == h && _outd == d && _outc / out_elempack == channels && out_elempack == packn)
            {
                top_blob = bottom_blob;
                return 0;
            }

            if (_coffset % packn == 0 && out_elempack == packn)
            {
                const Mat bottom_blob_sliced = bottom_blob.channel_range(_coffset / out_elempack, _outc / out_elempack);

                if (_outw == w && _outh == h && _outd == d)
                {
                    top_blob = bottom_blob_sliced.clone(opt.blob_allocator);
                    if (top_blob.empty())
                        return -100;
                }

                top_blob.create(_outw, _outh, _outd, _outc / out_elempack, out_elemsize, out_elempack, opt.blob_allocator);
                if (top_blob.empty())
                    return -100;

                #pragma omp parallel for num_threads(opt.num_threads)
                for (int q = 0; q < top_blob.c; q++)
                {
                    for (int z = 0; z < _outd; z++)
                    {
                        const Mat m = bottom_blob_sliced.channel(q).depth(z + _doffset);
                        Mat borderm = top_blob.channel(q).depth(z);

                        if (elembits == 16)
                            crop_packn_bf16_fp16s_rvv(m, borderm, _hoffset, _woffset, packn);
                        else
                            crop_packn_rvv(m, borderm, _hoffset, _woffset, packn);
                    }
                }

                return 0;
            }
        }
    }
#endif // __riscv_vector

    std::vector<Mat> bottom_blobs_unpacked(bottom_blobs.size());
    for (size_t i = 0; i < bottom_blobs.size(); i++)
    {
        Mat bottom_blob_unpacked = bottom_blobs[i];
        if (elempack != 1)
        {
            Option opt_pack1 = opt;
            opt_pack1.blob_allocator = opt.workspace_allocator;

            convert_packing(bottom_blobs[i], bottom_blob_unpacked, 1, opt_pack1);
            if (bottom_blob_unpacked.empty())
                return -100;
        }

        bottom_blobs_unpacked[i] = bottom_blob_unpacked;
    }

    return Crop::forward(bottom_blobs_unpacked, top_blobs, opt);
}

} // namespace ncnn
