// Copyright 2021 Tencent
// SPDX-License-Identifier: BSD-3-Clause

static void im2col_sgemm_pack1ton_rvv(const Mat& bottom_im2col, Mat& top_blob, const Mat& kernel, const Mat& _bias, const Option& opt)
{
    const int packn = csrr_vlenb() / 4;
    const size_t vl = __riscv_vsetvl_e32m1(packn);

    // Mat bottom_im2col(size, maxk, inch, 4u, 1, opt.workspace_allocator);

    const int size = bottom_im2col.w;
    const int maxk = bottom_im2col.h;
    const int inch = bottom_im2col.c;

    const int outch = top_blob.c;

    const float* bias = _bias;

    // permute
    Mat tmp;
    tmp.create(maxk, inch, size, 4u, 1, opt.workspace_allocator);
    {
        #pragma omp parallel for num_threads(opt.num_threads)
        for (int i = 0; i < size; i++)
        {
            float* tmpptr = tmp.channel(i);

            for (int q = 0; q < inch; q++)
            {
                const float* img0 = (const float*)bottom_im2col.channel(q) + i;

                for (int k = 0; k < maxk; k++)
                {
                    tmpptr[0] = img0[0];
                    img0 += size;
                    tmpptr += 1;
                }
            }
        }
    }

    #pragma omp parallel for num_threads(opt.num_threads)
    for (int p = 0; p < outch; p++)
    {
        float* outptr0 = top_blob.channel(p);

        int i = 0;
        for (; i < size; i++)
        {
            const float* tmpptr = tmp.channel(i);
            const float* kptr0 = kernel.channel(p);

            int nn = inch * maxk; // inch always > 0

            vfloat32m1_t _sum = __riscv_vfmv_v_f_f32m1(0.f, vl);

            if (bias)
            {
                _sum = __riscv_vle32_v_f32m1(bias + p * packn, vl);
            }

            for (int j = 0; j < nn; j++)
            {
                float val = *tmpptr++;
                vfloat32m1_t _w0 = __riscv_vle32_v_f32m1(kptr0, vl);
                _sum = __riscv_vfmacc_vf_f32m1(_sum, val, _w0, vl);

                kptr0 += packn;
            }

            __riscv_vse32_v_f32m1(outptr0, _sum, vl);

            outptr0 += packn;
        }
    }
}

static void convolution_im2col_sgemm_pack1ton_rvv(const Mat& bottom_blob, Mat& top_blob, const Mat& kernel, const Mat& _bias, int kernel_w, int kernel_h, int dilation_w, int dilation_h, int stride_w, int stride_h, const Option& opt)
{
    int w = bottom_blob.w;
    int inch = bottom_blob.c;

    int outw = top_blob.w;
    int outh = top_blob.h;
    const int size = outw * outh;

    const int maxk = kernel_w * kernel_h;

    // im2col
    Mat bottom_im2col(size, maxk, inch, 4u, 1, opt.workspace_allocator);
    {
        const int gap = w * stride_h - outw * stride_w;

        #pragma omp parallel for num_threads(opt.num_threads)
        for (int p = 0; p < inch; p++)
        {
            const Mat img = bottom_blob.channel(p);
            float* ptr = bottom_im2col.channel(p);

            for (int u = 0; u < kernel_h; u++)
            {
                for (int v = 0; v < kernel_w; v++)
                {
                    const float* sptr = img.row(dilation_h * u) + dilation_w * v;

                    for (int i = 0; i < outh; i++)
                    {
                        int j = 0;
                        for (; j < outw; j++)
                        {
                            ptr[0] = sptr[0];

                            sptr += stride_w;
                            ptr += 1;
                        }

                        sptr += gap;
                    }
                }
            }
        }
    }

    im2col_sgemm_pack1ton_rvv(bottom_im2col, top_blob, kernel, _bias, opt);
}
