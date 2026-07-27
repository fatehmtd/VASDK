#pragma once

#include "FrameProcessor.h"
#include "Frame.h"
#include <memory>
#include <string>

namespace va {
    namespace pipeline {

        /**
         * @brief Pipeline entry point for externally-originated triggers (e.g. a UI
         *        button), positioned right after the audio input processor.
         *
         * trigger() is safe to call from any thread - including a UI/JNI thread that
         * is not this processor's own processingLoop() thread - because it goes
         * through the normal FrameProcessor queue (enqueueFrame) instead of touching
         * any other processor's internals directly. The resulting TriggerFrame then
         * flows downstream through VAD / turn detection / agent state exactly like
         * any other pipeline frame.
         */
        class VASDK_EXPORT TriggerProcessor : public FrameProcessor {
        public:
            std::string getName() const override { return "TriggerProcessor"; }

            void processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) override {
                if (direction == FlowDirection::DOWNSTREAM) {
                    pushDownstream(frame);
                } else {
                    pushUpstream(frame);
                }
            }

            /**
             * @brief Inject a trigger into the pipeline. Thread-safe.
             */
            void trigger(transport::TriggerType type, const std::string& data = "", float confidence = 0.0f) {
                auto frame = std::make_shared<transport::TriggerFrame>();
                frame->triggerType = type;
                frame->triggerData = data;
                frame->confidence  = confidence;
                enqueueFrame(std::move(frame), FlowDirection::DOWNSTREAM);
            }
        };

    } // namespace pipeline
} // namespace va
