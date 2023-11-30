/*
 * Copyright (c) 2019, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#ifndef AOM_AV1_ENCODER_PARTITION_CNN_H_
#define AOM_AV1_ENCODER_PARTITION_CNN_H_

#ifdef __cplusplus
extern "C" {
#endif

#define CNN_BRANCH_0_OUT_CH 20
#define CNN_BRANCH_1_OUT_CH 4
#define CNN_BRANCH_2_OUT_CH 20
#define CNN_BRANCH_3_OUT_CH 20
#define CNN_TOT_OUT_CH                                                      \
  (((CNN_BRANCH_0_OUT_CH) + (CNN_BRANCH_1_OUT_CH) + (CNN_BRANCH_2_OUT_CH) + \
    (CNN_BRANCH_3_OUT_CH)))
#define CNN_BRANCH_0_OUT_SIZE (CNN_BRANCH_0_OUT_CH)
#define CNN_BRANCH_1_OUT_SIZE ((CNN_BRANCH_1_OUT_CH)*2 * 2)
#define CNN_BRANCH_2_OUT_SIZE ((CNN_BRANCH_2_OUT_CH)*4 * 4)
#define CNN_BRANCH_3_OUT_SIZE ((CNN_BRANCH_3_OUT_CH)*8 * 8)
#define CNN_OUT_BUF_SIZE                                \
  (((CNN_BRANCH_0_OUT_SIZE) + (CNN_BRANCH_1_OUT_SIZE) + \
    (CNN_BRANCH_2_OUT_SIZE) + (CNN_BRANCH_3_OUT_SIZE)))

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AOM_AV1_ENCODER_PARTITION_CNN_H_