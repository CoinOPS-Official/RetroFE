
#include "RestrictorManager.h"
#include "../../Utility/ThreadPool.h" // Include your thread pool
#include "../../Utility/Log.h"

     static constexpr char COMPONENT[] = "RestrictorManager";
     IRestrictor* RestrictorManager::gRestrictor = nullptr;

 RestrictorManager::RestrictorManager() = default;
 RestrictorManager::~RestrictorManager() = default; // The future and unique_ptr clean up themselves.

 void RestrictorManager::startInitialization() {
     // If the future is already valid, we've already started.
     if (restrictorFuture_.valid()) {
         return;
     }
     LOG_INFO(COMPONENT, "Enqueuing restrictor hardware detection to thread pool...");
     restrictorFuture_ = ThreadPool::getInstance().enqueue(&IRestrictor::create);
 }

 bool RestrictorManager::isReady() {
     if (!restrictorFuture_.valid()) {
         return false;
     }
     auto status = restrictorFuture_.wait_for(std::chrono::seconds(0));
     if (status == std::future_status::ready) {
         if (!restrictor_) {
             restrictor_ = restrictorFuture_.get(); // Move the result from the future
             if (restrictor_) {
                 LOG_INFO(COMPONENT, "Restrictor hardware detection complete. Device found.");
                 gRestrictor = restrictor_.get(); // Set the global pointer
             }
             else {
                 LOG_INFO(COMPONENT, "Restrictor hardware detection complete. No device found.");
                 gRestrictor = nullptr;
             }
         }
         return true;
     }
     return false;
 }

 IRestrictor* RestrictorManager::getGlobalRestrictor() {
     return gRestrictor;
 }