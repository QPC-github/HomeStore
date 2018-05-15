#include <cassert>
#include <cstdio>
#include <ctime>
extern "C" {
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timeb.h>
}
#include <atomic>
#include <iostream>
		
#include <iomgr/iomgr.hpp>
#include <sds_logging/logging.h>

#include "device/device.h"
#include "device/virtual_dev.hpp"
#include "volume.hpp"

using namespace std; 
using namespace homestore;
using namespace homeio;

static size_t const page_size = sysconf(_SC_PAGESIZE);

INIT_VMODULES(BTREE_VMODULES);

using log_level = spdlog::level::level_enum;

static std::shared_ptr<spdlog::logger> logger_;

namespace sds_logging {
std::shared_ptr<spdlog::logger> GetLogger() {
   return logger_;
}
}

homestore::DeviceManager *dev_mgr = nullptr;
homestore::Volume *vol;

constexpr auto MAX_OUTSTANDING_IOs = 64u;
constexpr auto MAX_CNT_THREAD = 8u;
constexpr auto MAX_THREADS = 8u;

constexpr auto Ki = 1024ull;
constexpr auto Mi = Ki * Ki;
constexpr auto Gi = Ki * Mi;
constexpr auto WRITE_SIZE = 8 * Ki;
constexpr auto BUF_SIZE = WRITE_SIZE / (8 * Ki);
constexpr auto MAX_BUF = (8 * Mi) / WRITE_SIZE;
constexpr auto MAX_VOL_SIZE = (256 * Mi);
constexpr auto MAX_READ = MAX_BUF ;

int is_random_read = false;
int is_random_write = false;
bool is_read = true;
bool is_write = true;

uint8_t *bufs[MAX_BUF];
boost::intrusive_ptr<homestore::BlkBuffer>boost_buf[MAX_BUF];

/* change it to atomic counters */
std::atomic<uint64_t> read_cnt(0);
std::atomic<uint64_t> write_cnt(0);
homeio::Clock::time_point read_startTime;
homeio::Clock::time_point write_startTime;

uint64_t get_elapsed_time(homeio::Clock::time_point startTime) 
{
   std::chrono::nanoseconds ns = std::chrono::duration_cast
       < std::chrono::nanoseconds >(homeio::Clock::now() - startTime);
   return ns.count() / 1000;
}


std::atomic<size_t> outstanding_ios(0);
class test_ep : iomgr::EndPoint {
   struct req:volume_req {
      int indx;
   };
 public:
   int const ev_fd;
   struct thread_info {
      int outstanding_ios_per_thread;
   };

   static thread_local thread_info info;
   void process_ev_common(int fd, void *cookie, int event) {
      uint64_t temp;
      (void) read(ev_fd, &temp, sizeof(uint64_t));

      iomgr->process_done(fd, event);
      if ((atomic_load(&outstanding_ios) + MAX_CNT_THREAD) < MAX_OUTSTANDING_IOs) {
         /* raise an event */
         iomgr->fd_reschedule(fd, event);
      }

      size_t cnt = 0;
      while (atomic_load(&outstanding_ios) < MAX_OUTSTANDING_IOs && cnt < MAX_CNT_THREAD) {
         size_t temp;
         ++outstanding_ios;
         if((temp = write_cnt.fetch_add(1, std::memory_order_relaxed)) < MAX_BUF) {
            if (temp == 1) {
               write_startTime = homeio::Clock::now();
            }
            writefunc(temp);
         } else if (is_read &&
                    (temp = read_cnt.fetch_add(1, std::memory_order_relaxed)) < MAX_READ) {
            if (temp == 1) {
               read_startTime = homeio::Clock::now();
            }
            readfunc(temp);
         }
         ++cnt;
         assert(outstanding_ios != SIZE_MAX);
      }
   }

   test_ep(iomgr::ioMgr *iomgr) :
       iomgr::EndPoint(iomgr),
       ev_fd(eventfd(0, EFD_NONBLOCK))
   {
      iomgr->add_fd(ev_fd,
                    [this] (auto fd, auto cookie, auto event) { process_ev_common(fd, cookie, event); },
                    EPOLLIN,
                    9,
                    NULL);

      iomgr->add_ep(this);

      /* Create a volume */
      vol = new homestore::Volume(dev_mgr,
                                  MAX_VOL_SIZE, 
                                  [this] (auto status, auto vol_req) { process_completions(status, vol_req); });
      LOGINFO("Created volume of size: {}", MAX_VOL_SIZE);
   }

   void writefunc(int const cnt) {
      struct req *req = new struct req();
      req->is_read = false;
      if (is_random_write) {
         assert(!is_read);
         uint64_t random = rand();
         uint64_t i = (random % (MAX_BUF));
         boost_buf[cnt] = vol->write(i * BUF_SIZE, bufs[i], BUF_SIZE, req);
         /* store intrusive buffer pointer */
      } else {
         std::vector<boost::intrusive_ptr< BlkBuffer >> buf_list;
         boost_buf[cnt] = vol->write(cnt * BUF_SIZE, bufs[cnt], BUF_SIZE, req);
      }
   }

   void readfunc(int const cnt) {
      if (!is_read) {
         return;
      }
      assert(is_write);
      //		 struct req *req = (struct req *)malloc(sizeof(struct req));
      struct req *req = new struct req();
      req->is_read = true;
      if (is_random_read) {
         uint64_t random = rand();
         uint64_t i = random % MAX_BUF;
         req->indx = i;
         vol->read(i * BUF_SIZE, BUF_SIZE, req);
      } else {
         req->indx = cnt;
         vol->read(cnt * BUF_SIZE, BUF_SIZE, req);
      }
   }

   void process_completions(int status, volume_req *vol_req) {
      assert(status == 0);
      struct req * req = static_cast< struct req* >(vol_req);
      /* raise an event */
      uint64_t temp = 1;
      outstanding_ios--;
      assert(outstanding_ios != SIZE_MAX);
      uint64_t size = write(ev_fd, &temp, sizeof(uint64_t));
      if (size != sizeof(uint64_t)) {
         assert(0);
      }
      if (req->is_read) {
         /* memcmp */
#ifndef NDEBUG
         homeds::blob b  = req->read_buf_list[0]->at_offset(0);	
         assert(b.size == BUF_SIZE * 8192);
         int j = memcmp((void *)b.bytes, (void *)bufs[req->indx], b.size);
         assert(j == 0);
#endif
      }
      delete(req);
   }

   void init_local() override {
   }
   void print_perf() override {
   }
};

thread_local test_ep::thread_info test_ep::info = {0};

INIT_VMODULES(CACHE_VMODULES);
int main(int argc, char** argv) {
   //        InithomedsLogging(0, CACHE_VMODULES);
   std::vector<std::string> dev_names;
   bool create = ((argc > 1) && (!strcmp(argv[1], "-c")));

   for (auto i : boost::irange(create ? 2 : 1, argc)) {
      dev_names.emplace_back(argv[i]);
   }

   spdlog::set_async_mode(4096, spdlog::async_overflow_policy::block_retry, nullptr, std::chrono::seconds(2));
   spdlog::set_pattern("[%D %H:%M:%S.%f] [%l] [%t] %v");
   spdlog::set_level(log_level::info);
   logger_ = spdlog::stdout_color_mt("example");

   /* create iomgr */
   iomgr::ioMgr iomgr(2, MAX_THREADS);

   /* Create/Load the devices */
   LOGINFO("Creating devices.");
   dev_mgr = new homestore::DeviceManager(Volume::new_vdev_found,
                                          0,
                                          &iomgr,
                                          virtual_dev_process_completions);
   try {
      dev_mgr->add_devices(dev_names);
   } catch (std::exception &e) {
      LOG(INFO) << "Exception info " << e.what();
      exit(1);
   }

   /* create endpoint */
   iomgr.start();
   test_ep ep(&iomgr);

   /* create dataset */
   auto devs = dev_mgr->get_all_devices();
   LOGINFO("Creating dataset.");
   for (auto i = 0u; i < MAX_BUF; i++) {
      //		bufs[i] = new uint8_t[8192*1000]();
      if (auto ec = posix_memalign((void**)&bufs[i], page_size, 8192 * BUF_SIZE))
         throw std::system_error(std::error_code(ec, std::generic_category()));
      uint8_t *bufp = bufs[i];
      for (auto j = 0u; j < (8192 * BUF_SIZE/8); j++) {
         memset(bufp, i + j + 1 , 8);
         bufp = bufp + 8;
      }
   }

   LOGINFO("Initializing performance counters.");
   vol->init_perf_cntrs();

   /* send an event */
   uint64_t temp = 1;
   (void) write(ep.ev_fd, &temp, sizeof(uint64_t));

   LOGINFO("Waiting for writes to finish.");
   while(atomic_load(&write_cnt) < MAX_BUF) ;

   uint64_t time_us = get_elapsed_time(write_startTime);
   printf("write counters..........\n");
   printf("total writes %lu\n", atomic_load(&write_cnt));
   printf("total time spent %lu us\n", time_us);
   printf("total time spend per io %lu us\n", time_us/atomic_load(&write_cnt));
   printf("iops %lu\n",(atomic_load(&write_cnt) * 1000 * 1000)/time_us);

   LOGINFO("Waiting for reads to finish.");
   while(is_read && atomic_load(&read_cnt) < MAX_READ) {
   }

   time_us = get_elapsed_time(read_startTime);
   printf("read counters..........\n");
   printf("total reads %lu\n", atomic_load(&read_cnt));
   printf("total time spent %lu us\n", time_us);
   if (read_cnt)
      printf("total time spend per io %lu us\n", time_us/read_cnt);
   printf("iops %lu \n", (read_cnt * 1000 * 1000)/time_us);
   printf("additional counters.........\n");	
   vol->print_perf_cntrs();
   iomgr.print_perf_cntrs();
   LOGINFO("Complete");
}
