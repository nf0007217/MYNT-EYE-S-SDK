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
#include "mynteye/api/synthetic.h"

#include <algorithm>
#include <functional>
#include <stdexcept>

#include <opencv2/imgproc/imgproc.hpp>

#include "mynteye/logger.h"
#include "mynteye/api/object.h"
#include "mynteye/api/plugin.h"
#include "mynteye/api/processor.h"
#include "mynteye/api/processor/disparity_normalized_processor.h"
#include "mynteye/api/processor/disparity_processor.h"
#include "mynteye/api/processor/root_camera_processor.h"
#include "mynteye/api/processor/rectify_processor_ocv.h"
#include "mynteye/api/processor/depth_processor_ocv.h"
#include "mynteye/api/processor/points_processor_ocv.h"
#include "mynteye/api/config.h"
#ifdef WITH_CAM_MODELS
#include "mynteye/api/processor/depth_processor.h"
#include "mynteye/api/processor/points_processor.h"
#include "mynteye/api/processor/rectify_processor.h"
#endif
#include "mynteye/device/device.h"

#define RECTIFY_PROC_PERIOD 0
#define DISPARITY_PROC_PERIOD 0
#define DISPARITY_NORM_PROC_PERIOD 0
#define POINTS_PROC_PERIOD 0
#define DEPTH_PROC_PERIOD 0
#define ROOT_PROC_PERIOD 0

MYNTEYE_BEGIN_NAMESPACE

namespace {

cv::Mat frame2mat(const std::shared_ptr<device::Frame> &frame) {
  if (frame->format() == Format::YUYV) {
    cv::Mat img(frame->height(), frame->width(), CV_8UC2, frame->data());
    cv::cvtColor(img, img, cv::COLOR_YUV2BGR_YUY2);
    return img;
  } else if (frame->format() == Format::BGR888) {
    cv::Mat img(frame->height(), frame->width(), CV_8UC3, frame->data());
    return img;
  } else {  // Format::GRAY
    return cv::Mat(frame->height(), frame->width(), CV_8UC1, frame->data());
  }
}

api::StreamData data2api(const device::StreamData &data) {
  return {data.img, frame2mat(data.frame), data.frame, data.frame_id};
}

void process_childs(
    const std::shared_ptr<Processor> &proc, const std::string &name,
    const Object &obj) {
  auto &&processor = find_processor<Processor>(proc, name);
  for (auto child : processor->GetChilds()) {
    child->Process(obj);
  }
}

// ObjMat/ObjMat2 > api::StreamData

api::StreamData obj_data_first(const ObjMat2 *obj) {
  return {obj->first_data, obj->first, nullptr, obj->first_id};
}

api::StreamData obj_data_second(const ObjMat2 *obj) {
  return {obj->second_data, obj->second, nullptr, obj->second_id};
}

api::StreamData obj_data(const ObjMat *obj) {
  return {obj->data, obj->value, nullptr, obj->id};
}

api::StreamData obj_data_first(const std::shared_ptr<ObjMat2> &obj) {
  return {obj->first_data, obj->first, nullptr, obj->first_id};
}

api::StreamData obj_data_second(const std::shared_ptr<ObjMat2> &obj) {
  return {obj->second_data, obj->second, nullptr, obj->second_id};
}

api::StreamData obj_data(const std::shared_ptr<ObjMat> &obj) {
  return {obj->data, obj->value, nullptr, obj->id};
}

// api::StreamData > ObjMat/ObjMat2

ObjMat data_obj(const api::StreamData &data) {
  return ObjMat{data.frame, data.frame_id, data.img};
}

ObjMat2 data_obj(const api::StreamData &first, const api::StreamData &second) {
  return ObjMat2{
      first.frame, first.frame_id, first.img,
      second.frame, second.frame_id, second.img};
}

}  // namespace

void Synthetic::InitCalibInfo() {
  if (calib_model_ == CalibrationModel::PINHOLE) {
    LOG(INFO) << "camera calib model: pinhole";
    intr_left_ = api_->GetIntrinsicsBase(Stream::LEFT);
    intr_right_ = api_->GetIntrinsicsBase(Stream::RIGHT);
    extr_ =  std::make_shared<Extrinsics>(
        api_->GetExtrinsics(Stream::LEFT, Stream::RIGHT));
#ifdef WITH_CAM_MODELS
  } else if (calib_model_ == CalibrationModel::KANNALA_BRANDT) {
    LOG(INFO) << "camera calib model: kannala_brandt";
    intr_left_ = api_->GetIntrinsicsBase(Stream::LEFT);
    intr_right_ = api_->GetIntrinsicsBase(Stream::RIGHT);
    extr_ =  std::make_shared<Extrinsics>(
        api_->GetExtrinsics(Stream::LEFT, Stream::RIGHT));
#endif
  } else {
    calib_default_tag_ = true;
    calib_model_ = CalibrationModel::PINHOLE;
    LOG(INFO) << "camera calib model: unknow ,use default pinhole data";
    intr_left_ = getDefaultIntrinsics();
    intr_right_ = getDefaultIntrinsics();
    extr_ =  getDefaultExtrinsics();
  }
}

Synthetic::Synthetic(API *api, CalibrationModel calib_model)
    : api_(api),
      plugin_(nullptr),
      calib_model_(calib_model),
      calib_default_tag_(false),
      stream_data_listener_(nullptr) {
  VLOG(2) << __func__;
  CHECK_NOTNULL(api_);
  InitCalibInfo();
  InitProcessors();
  InitStreamSupports();
}

Synthetic::~Synthetic() {
  VLOG(2) << __func__;
  if (processor_) {
    processor_->Deactivate(true);
    processor_ = nullptr;
  }
}

void Synthetic::SetStreamDataListener(stream_data_listener_t listener) {
  stream_data_listener_ = listener;
}

void Synthetic::NotifyImageParamsChanged() {
  if (!calib_default_tag_) {
    intr_left_ = api_->GetIntrinsicsBase(Stream::LEFT);
    intr_right_ = api_->GetIntrinsicsBase(Stream::RIGHT);
    extr_ =  std::make_shared<Extrinsics>(
        api_->GetExtrinsics(Stream::LEFT, Stream::RIGHT));
  }
  if (calib_model_ ==  CalibrationModel::PINHOLE) {
    auto &&processor = find_processor<RectifyProcessorOCV>(processor_);
    if (processor) processor->ReloadImageParams(intr_left_, intr_right_, extr_);
#ifdef WITH_CAM_MODELS
  } else if (calib_model_ == CalibrationModel::KANNALA_BRANDT) {
    auto &&processor = find_processor<RectifyProcessor>(processor_);
    if (processor) processor->ReloadImageParams(intr_left_, intr_right_, extr_);
#endif
  } else {
    LOG(ERROR) << "Unknow calib model type in device: "
              << calib_model_ << ", use default pinhole model";
    auto &&processor = find_processor<RectifyProcessorOCV>(processor_);
    if (processor) processor->ReloadImageParams(intr_left_, intr_right_, extr_);
  }
}

const struct Synthetic::stream_control_t Synthetic::getControlDateWithStream(
    const Stream& stream) const {
  for (auto &&it : processors_) {
    for (auto it_s : it->getTargetStreams()) {
      if (it_s.stream == stream) {
        return it_s;
      }
    }
  }
  LOG(ERROR) << "ERROR: no suited processor for stream "<< stream;
  return {};
}

std::shared_ptr<Processor> Synthetic::getProcessorWithStream(
    const Stream& stream) {
  for (auto &&it : processors_) {
    for (auto it_s : it->getTargetStreams()) {
      if (it_s.stream == stream) {
        return it;
      }
    }
  }
  LOG(ERROR) << "ERROR: no suited processor for stream "<< stream;
}

void Synthetic::setControlDateCallbackWithStream(
    const struct stream_control_t& ctr_data) {
  for (auto &&it : processors_) {
    int i = 0;
    for (auto it_s : it->getTargetStreams()) {
      if (it_s.stream == ctr_data.stream) {
        it->target_streams_[i].stream_callback = ctr_data.stream_callback;
        return;
      }
      i++;
    }
  }
  LOG(ERROR) << "ERROR: no suited processor for stream "<< ctr_data.stream;
}

bool Synthetic::checkControlDateWithStream(const Stream& stream) const {
  for (auto &&it : processors_) {
    for (auto it_s : it->getTargetStreams()) {
      if (it_s.stream == stream) {
        return true;
      }
    }
  }
  return false;
}

bool Synthetic::Supports(const Stream &stream) const {
  return checkControlDateWithStream(stream);
}

Synthetic::mode_t Synthetic::SupportsMode(const Stream &stream) const {
  if (checkControlDateWithStream(stream)) {
    auto data = getControlDateWithStream(stream);
    return data.support_mode_;
  }
  return MODE_LAST;
}

void Synthetic::EnableStreamData(
    const Stream &stream, stream_switch_callback_t callback,
    bool try_tag) {
  // Activate processors of synthetic stream
  auto processor = getProcessorWithStream(stream);
  iterate_processor_CtoP_before(processor,
      [callback, try_tag](std::shared_ptr<Processor> proce){
        auto streams = proce->getTargetStreams();
        int act_tag = 0;
        for (unsigned int i = 0; i < proce->getStreamsSum() ; i++) {
          if (proce->target_streams_[i].enabled_mode_ == MODE_LAST) {
            callback(proce->target_streams_[i].stream);
            if (!try_tag) {
              act_tag++;
              proce->target_streams_[i].enabled_mode_ = MODE_SYNTHETIC;
            }
          }
        }
        if (act_tag > 0 && !proce->IsActivated()) {
          // std::cout << proce->Name() << " Active now" << std::endl;
          proce->Activate();
        }
      });
}
void Synthetic::DisableStreamData(
    const Stream &stream, stream_switch_callback_t callback,
    bool try_tag) {
  auto processor = getProcessorWithStream(stream);
  iterate_processor_PtoC_before(processor,
      [callback, try_tag](std::shared_ptr<Processor> proce){
        auto streams = proce->getTargetStreams();
        int act_tag = 0;
        for (unsigned int i = 0; i < proce->getStreamsSum() ; i++) {
          if (proce->target_streams_[i].enabled_mode_ == MODE_SYNTHETIC) {
            callback(proce->target_streams_[i].stream);
            if (!try_tag) {
              act_tag++;
              proce->target_streams_[i].enabled_mode_ = MODE_LAST;
            }
          }
        }
        if (act_tag > 0 && proce->IsActivated()) {
          // std::cout << proce->Name() << "Deactive now" << std::endl;
          proce->Deactivate();
        }
      });
}

void Synthetic::EnableStreamData(const Stream &stream) {
  EnableStreamData(stream, [](const Stream &stream){
        // std::cout << stream << "enabled in callback" << std::endl;
        MYNTEYE_UNUSED(stream);
      }, false);
}

void Synthetic::DisableStreamData(const Stream &stream) {
  DisableStreamData(stream, [](const Stream &stream){
        // std::cout << stream << "disabled in callback" << std::endl;
        MYNTEYE_UNUSED(stream);
      }, false);
}

bool Synthetic::IsStreamDataEnabled(const Stream &stream) const {
  if (checkControlDateWithStream(stream)) {
    auto data = getControlDateWithStream(stream);
    return data.enabled_mode_ == MODE_SYNTHETIC ||
        data.enabled_mode_ == MODE_NATIVE;
  }
  return false;
}

void Synthetic::SetStreamCallback(
    const Stream &stream, stream_callback_t callback) {
  stream_control_t data;
  data.stream = stream;
  if (callback == nullptr) {
    data.stream_callback = nullptr;
  } else {
    data.stream_callback = callback;
  }
  setControlDateCallbackWithStream(data);
}

bool Synthetic::HasStreamCallback(const Stream &stream) const {
  if (checkControlDateWithStream(stream)) {
    auto data = getControlDateWithStream(stream);
    if (data.stream_callback != nullptr) {
      return true;
    }
  }
  return false;
}

void Synthetic::StartVideoStreaming() {
  auto &&device = api_->device();
  for (unsigned int i =0; i< processors_.size(); i++) {
    auto streams = processors_[i]->getTargetStreams();
    for (unsigned int j =0; j< streams.size(); j++) {
      if (processors_[i]->target_streams_[j].support_mode_ == MODE_NATIVE) {
        auto stream = processors_[i]->target_streams_[j].stream;
        device->SetStreamCallback(
          stream,
          [this, stream](const device::StreamData &data) {
            auto &&stream_data = data2api(data);
            ProcessNativeStream(stream, stream_data);
            // Need mutex if set callback after start
            if (HasStreamCallback(stream)) {
              auto data = getControlDateWithStream(stream);
              data.stream_callback(stream_data);
            }
          },
          true);
      }
    }
  }
  device->Start(Source::VIDEO_STREAMING);
}

void Synthetic::StopVideoStreaming() {
  auto &&device = api_->device();
  for (unsigned int i =0; i< processors_.size(); i++) {
    auto streams = processors_[i]->getTargetStreams();
    for (unsigned int j =0; j< streams.size(); j++) {
      if (processors_[i]->target_streams_[j].support_mode_ == MODE_NATIVE) {
        auto stream = processors_[i]->target_streams_[j].stream;
        device->SetStreamCallback(stream, nullptr);
      }
    }
  }
  device->Stop(Source::VIDEO_STREAMING);
}

void Synthetic::WaitForStreams() {
  api_->device()->WaitForStreams();
}

api::StreamData Synthetic::GetStreamData(const Stream &stream) {
  auto &&mode = GetStreamEnabledMode(stream);
  if (mode == MODE_NATIVE) {
    auto &&device = api_->device();
    return data2api(device->GetStreamData(stream));
  } else if (mode == MODE_SYNTHETIC) {
    auto processor = getProcessorWithStream(stream);
    auto sum = processor->getStreamsSum();
    auto &&out = processor->GetOutput();
    static std::shared_ptr<ObjMat2> output = nullptr;
    if (sum == 1) {
      if (out != nullptr) {
        auto &&output = Object::Cast<ObjMat>(out);
        if (output != nullptr) {
          return obj_data(output);
        }
        VLOG(2) << "Rectify not ready now";
      }
    } else if (sum == 2) {
      if (out != nullptr) {
        output = Object::Cast<ObjMat2>(out);
      }
      auto streams = processor->getTargetStreams();
      if (output != nullptr) {
        int num = 0;
        for (auto it : streams) {
          if (it.stream == stream) {
            if (num == 1) {
              return obj_data_first(output);
            } else {
              return obj_data_second(output);
            }
          }
          num++;
        }
      }
      VLOG(2) << "Rectify not ready now";
    } else {
      LOG(ERROR) << "error: invalid sum!";
    }
    return {};  // frame.empty() == true
  } else {
    LOG(ERROR) << "Failed to get stream data of " << stream
               << ", unsupported or disabled";
    return {};  // frame.empty() == true
  }
}

std::vector<api::StreamData> Synthetic::GetStreamDatas(const Stream &stream) {
  auto &&mode = GetStreamEnabledMode(stream);
  if (mode == MODE_NATIVE) {
    auto &&device = api_->device();
    std::vector<api::StreamData> datas;
    for (auto &&data : device->GetStreamDatas(stream)) {
      datas.push_back(data2api(data));
    }
    return datas;
  } else if (mode == MODE_SYNTHETIC) {
    return {GetStreamData(stream)};
  } else {
    LOG(ERROR) << "Failed to get stream data of " << stream
               << ", unsupported or disabled";
  }
  return {};
}

void Synthetic::SetPlugin(std::shared_ptr<Plugin> plugin) {
  plugin_ = plugin;
}

bool Synthetic::HasPlugin() const {
  return plugin_ != nullptr;
}

void Synthetic::InitStreamSupports() {
  auto &&device = api_->device();
  if (device->Supports(Stream::LEFT) && device->Supports(Stream::RIGHT)) {
    auto processor = getProcessorWithStream(Stream::LEFT);
    for (unsigned int i = 0; i< processor->target_streams_.size(); i++) {
      if (processor->target_streams_[i].stream == Stream::LEFT) {
        processor->target_streams_[i].support_mode_ = MODE_NATIVE;
      }
      if (processor->target_streams_[i].stream == Stream::RIGHT) {
        processor->target_streams_[i].support_mode_ = MODE_NATIVE;
      }
    }

    std::vector<Stream> stream_chain{
        Stream::LEFT_RECTIFIED, Stream::RIGHT_RECTIFIED,
        Stream::DISPARITY,      Stream::DISPARITY_NORMALIZED,
        Stream::POINTS,         Stream::DEPTH};
    for (auto &&stream : stream_chain) {
      auto processor = getProcessorWithStream(stream);
      for (unsigned int i = 0; i< processor->target_streams_.size(); i++) {
        if (processor->target_streams_[i].stream == stream) {
          if (device->Supports(stream)) {
            processor->target_streams_[i].support_mode_ = MODE_NATIVE;
            processor->target_streams_[i].enabled_mode_ = MODE_NATIVE;
          } else {
            processor->target_streams_[i].support_mode_ = MODE_SYNTHETIC;
          }
        }
      }
    }
  }
}

Synthetic::mode_t Synthetic::GetStreamEnabledMode(const Stream &stream) const {
  if (checkControlDateWithStream(stream)) {
    auto data = getControlDateWithStream(stream);
    return data.enabled_mode_;
  }
  return MODE_LAST;
}

bool Synthetic::IsStreamEnabledNative(const Stream &stream) const {
  return GetStreamEnabledMode(stream) == MODE_NATIVE;
}

bool Synthetic::IsStreamEnabledSynthetic(const Stream &stream) const {
  return GetStreamEnabledMode(stream) == MODE_SYNTHETIC;
}

void Synthetic::InitProcessors() {
  std::shared_ptr<Processor> rectify_processor = nullptr;
#ifdef WITH_CAM_MODELS
  std::shared_ptr<RectifyProcessor> rectify_processor_imp = nullptr;
#endif
  cv::Mat Q;
  if (calib_model_ ==  CalibrationModel::PINHOLE) {
    auto &&rectify_processor_ocv =
        std::make_shared<RectifyProcessorOCV>(intr_left_, intr_right_, extr_,
                                              RECTIFY_PROC_PERIOD);
    Q = rectify_processor_ocv->Q;
    rectify_processor = rectify_processor_ocv;
#ifdef WITH_CAM_MODELS
  } else if (calib_model_ == CalibrationModel::KANNALA_BRANDT) {
    rectify_processor_imp =
        std::make_shared<RectifyProcessor>(intr_left_, intr_right_, extr_,
                                           RECTIFY_PROC_PERIOD);
    rectify_processor = rectify_processor_imp;
#endif
  } else {
    LOG(ERROR) << "Unknow calib model type in device: "
              << calib_model_ << ", use default pinhole model";
    auto &&rectify_processor_ocv =
        std::make_shared<RectifyProcessorOCV>(intr_left_, intr_right_, extr_,
                                              RECTIFY_PROC_PERIOD);
    rectify_processor = rectify_processor_ocv;
  }
  auto &&disparity_processor =
      std::make_shared<DisparityProcessor>(DisparityComputingMethod::SGBM,
                                           DISPARITY_PROC_PERIOD);
  auto &&disparitynormalized_processor =
      std::make_shared<DisparityNormalizedProcessor>(
          DISPARITY_NORM_PROC_PERIOD);
  std::shared_ptr<Processor> points_processor = nullptr;
  if (calib_model_ ==  CalibrationModel::PINHOLE) {
    points_processor = std::make_shared<PointsProcessorOCV>(
        Q, POINTS_PROC_PERIOD);
#ifdef WITH_CAM_MODELS
  } else if (calib_model_ == CalibrationModel::KANNALA_BRANDT) {
    points_processor = std::make_shared<PointsProcessor>(
        rectify_processor_imp -> getCalibInfoPair(),
        POINTS_PROC_PERIOD);
#endif
  } else {
    points_processor = std::make_shared<PointsProcessorOCV>(
        Q, POINTS_PROC_PERIOD);
  }
  std::shared_ptr<Processor> depth_processor = nullptr;
  if (calib_model_ ==  CalibrationModel::PINHOLE) {
    depth_processor = std::make_shared<DepthProcessorOCV>(DEPTH_PROC_PERIOD);
#ifdef WITH_CAM_MODELS
  } else if (calib_model_ == CalibrationModel::KANNALA_BRANDT) {
    depth_processor = std::make_shared<DepthProcessor>(
        rectify_processor_imp -> getCalibInfoPair(),
        DEPTH_PROC_PERIOD);
#endif
  } else {
    depth_processor = std::make_shared<DepthProcessorOCV>(DEPTH_PROC_PERIOD);
  }
  auto root_processor =
        std::make_shared<RootProcessor>(ROOT_PROC_PERIOD);
  root_processor->AddChild(rectify_processor);

  rectify_processor->addTargetStreams(
      {Stream::LEFT_RECTIFIED, Mode::MODE_LAST, Mode::MODE_LAST, nullptr});
  rectify_processor->addTargetStreams(
      {Stream::RIGHT_RECTIFIED, Mode::MODE_LAST, Mode::MODE_LAST, nullptr});
  disparity_processor->addTargetStreams(
      {Stream::DISPARITY, Mode::MODE_LAST, Mode::MODE_LAST, nullptr});
  disparitynormalized_processor->addTargetStreams(
      {Stream::DISPARITY_NORMALIZED, Mode::MODE_LAST, Mode::MODE_LAST, nullptr});
  points_processor->addTargetStreams(
      {Stream::POINTS, Mode::MODE_LAST, Mode::MODE_LAST, nullptr});
  depth_processor->addTargetStreams(
      {Stream::DEPTH, Mode::MODE_LAST, Mode::MODE_LAST, nullptr});
  root_processor->addTargetStreams(
      {Stream::LEFT, Mode::MODE_NATIVE, Mode::MODE_NATIVE, nullptr});
  root_processor->addTargetStreams(
      {Stream::RIGHT, Mode::MODE_NATIVE, Mode::MODE_NATIVE, nullptr});

  processors_.push_back(root_processor);
  processors_.push_back(rectify_processor);
  processors_.push_back(disparity_processor);
  processors_.push_back(disparitynormalized_processor);
  processors_.push_back(points_processor);
  processors_.push_back(depth_processor);
  using namespace std::placeholders;  // NOLINT
  rectify_processor->SetProcessCallback(
      std::bind(&Synthetic::OnRectifyProcess, this, _1, _2, _3));
  disparity_processor->SetProcessCallback(
      std::bind(&Synthetic::OnDisparityProcess, this, _1, _2, _3));
  disparitynormalized_processor->SetProcessCallback(
      std::bind(&Synthetic::OnDisparityNormalizedProcess, this, _1, _2, _3));
  points_processor->SetProcessCallback(
      std::bind(&Synthetic::OnPointsProcess, this, _1, _2, _3));
  depth_processor->SetProcessCallback(
      std::bind(&Synthetic::OnDepthProcess, this, _1, _2, _3));

  rectify_processor->SetPostProcessCallback(
      std::bind(&Synthetic::OnRectifyPostProcess, this, _1));
  disparity_processor->SetPostProcessCallback(
      std::bind(&Synthetic::OnDisparityPostProcess, this, _1));
  disparitynormalized_processor->SetPostProcessCallback(
      std::bind(&Synthetic::OnDisparityNormalizedPostProcess, this, _1));
  points_processor->SetPostProcessCallback(
      std::bind(&Synthetic::OnPointsPostProcess, this, _1));
  depth_processor->SetPostProcessCallback(
      std::bind(&Synthetic::OnDepthPostProcess, this, _1));

  if (calib_model_ == CalibrationModel::PINHOLE) {
    // PINHOLE
    rectify_processor->AddChild(disparity_processor);
    disparity_processor->AddChild(disparitynormalized_processor);
    disparity_processor->AddChild(points_processor);
    points_processor->AddChild(depth_processor);
  } else if (calib_model_ == CalibrationModel::KANNALA_BRANDT) {
    // KANNALA_BRANDT
    rectify_processor->AddChild(disparity_processor);
    disparity_processor->AddChild(disparitynormalized_processor);
    disparity_processor->AddChild(depth_processor);
    depth_processor->AddChild(points_processor);
  } else {
    // UNKNOW
    LOG(ERROR) << "Unknow calib model type in device: "
               << calib_model_;
  }

  processor_ = rectify_processor;
}

void Synthetic::ProcessNativeStream(
    const Stream &stream, const api::StreamData &data) {
  NotifyStreamData(stream, data);
  if (stream == Stream::LEFT || stream == Stream::RIGHT) {
    std::unique_lock<std::mutex> lk(mtx_left_right_ready_);
    static api::StreamData left_data, right_data;
    if (stream == Stream::LEFT) {
      left_data = data;
    } else if (stream == Stream::RIGHT) {
      right_data = data;
    }
    if (left_data.img && right_data.img &&
        left_data.img->frame_id == right_data.img->frame_id) {
      std::shared_ptr<Processor> processor = nullptr;
      if (calib_model_ ==  CalibrationModel::PINHOLE) {
        processor = find_processor<RectifyProcessorOCV>(processor_);
#ifdef WITH_CAM_MODELS
      } else if (calib_model_ == CalibrationModel::KANNALA_BRANDT) {
        processor = find_processor<RectifyProcessor>(processor_);
#endif
      } else {
        LOG(ERROR) << "Unknow calib model type in device: "
                  << calib_model_ << ", use default pinhole model";
        processor = find_processor<RectifyProcessorOCV>(processor_);
      }
      processor->Process(data_obj(left_data, right_data));
    }
    return;
  }

  if (stream == Stream::LEFT_RECTIFIED || stream == Stream::RIGHT_RECTIFIED) {
    static api::StreamData left_rect_data, right_rect_data;
    if (stream == Stream::LEFT_RECTIFIED) {
      left_rect_data = data;
    } else if (stream == Stream::RIGHT_RECTIFIED) {
      right_rect_data = data;
    }
    if (left_rect_data.img && right_rect_data.img &&
        left_rect_data.img->frame_id == right_rect_data.img->frame_id) {
      std::string name = RectifyProcessorOCV::NAME;
      if (calib_model_ ==  CalibrationModel::PINHOLE) {
        name = RectifyProcessorOCV::NAME;
#ifdef WITH_CAM_MODELS
      } else if (calib_model_ == CalibrationModel::KANNALA_BRANDT) {
        name = RectifyProcessor::NAME;
#endif
      }
      process_childs(processor_, name,
          data_obj(left_rect_data, right_rect_data));
    }
    return;
  }

  switch (stream) {
    case Stream::DISPARITY: {
      process_childs(processor_, DisparityProcessor::NAME, data_obj(data));
    } break;
    case Stream::DISPARITY_NORMALIZED: {
      process_childs(processor_, DisparityNormalizedProcessor::NAME,
          data_obj(data));
    } break;
    case Stream::POINTS: {
      if (calib_model_ == CalibrationModel::PINHOLE) {
        // PINHOLE
        process_childs(processor_, PointsProcessorOCV::NAME, data_obj(data));
#ifdef WITH_CAM_MODELS
      } else if (calib_model_ == CalibrationModel::KANNALA_BRANDT) {
        // KANNALA_BRANDT
        process_childs(processor_, PointsProcessor::NAME, data_obj(data));
#endif
      } else {
        // UNKNOW
        LOG(ERROR) << "Unknow calib model type in device: "
                  << calib_model_;
      }
    } break;
    case Stream::DEPTH: {
      if (calib_model_ == CalibrationModel::PINHOLE) {
        // PINHOLE
        process_childs(processor_, DepthProcessorOCV::NAME, data_obj(data));
#ifdef WITH_CAM_MODELS
      } else if (calib_model_ == CalibrationModel::KANNALA_BRANDT) {
        // KANNALA_BRANDT
        process_childs(processor_, DepthProcessor::NAME, data_obj(data));
#endif
      } else {
        // UNKNOW
        LOG(ERROR) << "Unknow calib model type in device: "
                  << calib_model_;
      }
    } break;
    default:
      break;
  }
}

bool Synthetic::OnRectifyProcess(
    Object *const in, Object *const out,
    std::shared_ptr<Processor> const parent) {
  MYNTEYE_UNUSED(parent)
  if (plugin_ && plugin_->OnRectifyProcess(in, out)) {
    return true;
  }
  return GetStreamEnabledMode(Stream::LEFT_RECTIFIED) != MODE_SYNTHETIC;
  // && GetStreamEnabledMode(Stream::RIGHT_RECTIFIED) != MODE_SYNTHETIC
}

bool Synthetic::OnDisparityProcess(
    Object *const in, Object *const out,
    std::shared_ptr<Processor> const parent) {
  MYNTEYE_UNUSED(parent)
  if (plugin_ && plugin_->OnDisparityProcess(in, out)) {
    return true;
  }
  return GetStreamEnabledMode(Stream::DISPARITY) != MODE_SYNTHETIC;
}

bool Synthetic::OnDisparityNormalizedProcess(
    Object *const in, Object *const out,
    std::shared_ptr<Processor> const parent) {
  MYNTEYE_UNUSED(parent)
  if (plugin_ && plugin_->OnDisparityNormalizedProcess(in, out)) {
    return true;
  }
  return GetStreamEnabledMode(Stream::DISPARITY_NORMALIZED) != MODE_SYNTHETIC;
}

bool Synthetic::OnPointsProcess(
    Object *const in, Object *const out,
    std::shared_ptr<Processor> const parent) {
  MYNTEYE_UNUSED(parent)
  if (plugin_ && plugin_->OnPointsProcess(in, out)) {
    return true;
  }
  return GetStreamEnabledMode(Stream::POINTS) != MODE_SYNTHETIC;
}

bool Synthetic::OnDepthProcess(
    Object *const in, Object *const out,
    std::shared_ptr<Processor> const parent) {
  MYNTEYE_UNUSED(parent)
  if (plugin_ && plugin_->OnDepthProcess(in, out)) {
    return true;
  }
  return GetStreamEnabledMode(Stream::DEPTH) != MODE_SYNTHETIC;
}

void Synthetic::OnRectifyPostProcess(Object *const out) {
  const ObjMat2 *output = Object::Cast<ObjMat2>(out);
  NotifyStreamData(Stream::LEFT_RECTIFIED, obj_data_first(output));
  NotifyStreamData(Stream::RIGHT_RECTIFIED, obj_data_second(output));
  if (HasStreamCallback(Stream::LEFT_RECTIFIED)) {
    auto data = getControlDateWithStream(Stream::LEFT_RECTIFIED);
    data.stream_callback(obj_data_first(output));
  }
  if (HasStreamCallback(Stream::RIGHT_RECTIFIED)) {
    auto data = getControlDateWithStream(Stream::RIGHT_RECTIFIED);
    data.stream_callback(obj_data_second(output));
  }
}

void Synthetic::OnDisparityPostProcess(Object *const out) {
  const ObjMat *output = Object::Cast<ObjMat>(out);
  NotifyStreamData(Stream::DISPARITY, obj_data(output));
  if (HasStreamCallback(Stream::DISPARITY)) {
    auto data = getControlDateWithStream(Stream::DISPARITY);
    data.stream_callback(obj_data(output));
  }
}

void Synthetic::OnDisparityNormalizedPostProcess(Object *const out) {
  const ObjMat *output = Object::Cast<ObjMat>(out);
  NotifyStreamData(Stream::DISPARITY_NORMALIZED, obj_data(output));
  if (HasStreamCallback(Stream::DISPARITY_NORMALIZED)) {
    auto data = getControlDateWithStream(Stream::DISPARITY_NORMALIZED);
    data.stream_callback(obj_data(output));
  }
}

void Synthetic::OnPointsPostProcess(Object *const out) {
  const ObjMat *output = Object::Cast<ObjMat>(out);
  NotifyStreamData(Stream::POINTS, obj_data(output));
  if (HasStreamCallback(Stream::POINTS)) {
    auto data = getControlDateWithStream(Stream::POINTS);
    data.stream_callback(obj_data(output));
  }
}

void Synthetic::OnDepthPostProcess(Object *const out) {
  const ObjMat *output = Object::Cast<ObjMat>(out);
  NotifyStreamData(Stream::DEPTH, obj_data(output));
  if (HasStreamCallback(Stream::DEPTH)) {
    auto data = getControlDateWithStream(Stream::DEPTH);
    data.stream_callback(obj_data(output));
  }
}

void Synthetic::SetDisparityComputingMethodType(
      const DisparityComputingMethod &MethodType) {
  if (checkControlDateWithStream(Stream::LEFT_RECTIFIED)) {
    auto processor = find_processor<DisparityProcessor>(processor_);
    if (processor)
      processor->SetDisparityComputingMethodType(MethodType);
    return;
  }
  LOG(ERROR) << "ERROR: no suited processor for disparity computing.";
}

void Synthetic::NotifyStreamData(
    const Stream &stream, const api::StreamData &data) {
  if (stream_data_listener_) {
    stream_data_listener_(stream, data);
  }
}

MYNTEYE_END_NAMESPACE
