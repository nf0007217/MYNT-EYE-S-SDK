// Copyright 2018 Slightech Co., Ltd. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#ifndef MYNTEYE_API_PROCESSOR_ROOT_CAMERA_PROCESSOR_H_
#define MYNTEYE_API_PROCESSOR_ROOT_CAMERA_PROCESSOR_H_
#pragma once

#include <string>

#include <opencv2/core/core.hpp>

#include "mynteye/api/processor.h"
#include "mynteye/logger.h"

MYNTEYE_BEGIN_NAMESPACE

class RootProcessor : public Processor {
 public:
  static const char NAME[];

  explicit RootProcessor(std::int32_t proc_period = 0);
  virtual ~RootProcessor();

  std::string Name() override;

 protected:
  Object *OnCreateOutput() override;
  bool OnProcess(
      Object *const in, Object *const out,
      std::shared_ptr<Processor> const parent) override;
};

MYNTEYE_END_NAMESPACE

#endif  // MYNTEYE_API_PROCESSOR_ROOT_CAMERA_PROCESSOR_H_
