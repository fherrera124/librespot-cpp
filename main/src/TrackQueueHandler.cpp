#include "TrackQueueHandler.h"
#include "events/EventLoop.h"

using namespace cspot;

namespace {
class DefaultTrackQueueHandler : public TrackQueueHandler {
 public:
  DefaultTrackQueueHandler(std::shared_ptr<SpClient> spClient,
                           std::shared_ptr<EventLoop> eventLoop,
                           uint32_t maxWindowSize = 33,
                           uint32_t trackUpdateThreshold = 8);

 private:
  const char* LOG_TAG = "TrackQueueHandler";
};
};  // namespace
