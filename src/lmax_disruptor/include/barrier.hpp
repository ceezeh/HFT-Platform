#pragma once
#include <atomic>
#include <functional>

// Barrier helps to ensure all processes arrive 
// at a certain point before continuing.
namespace barrier{
enum WAIT_STATUS{NONE, TIMEOUT, SUCCESS};

template<int count>
class Barrier{

    public:
        Barrier(): go_signal_(0), current_(count){}

        WAIT_STATUS Wait(std::function<void()> callback = nullptr) {
            
            // Attempt to atomically decrease.
            int current_seq =current_.fetch_sub(1, std::memory_order_acq_rel);
            --current_seq;
            // std::cout <<" Barrier check 0. Go Signal: "<< go_signal_.load(std::memory_order_acquire)<< ", current_seq: "<< current_seq <<std::endl;
            if (current_seq==go_signal_.load(std::memory_order_acquire)) {
                // Reset current
                if (callback) callback();
                current_.store( 2*count + go_signal_.load(std::memory_order_acquire), std::memory_order_release);
                go_signal_.fetch_add(count, std::memory_order_acq_rel);
                // std::cout <<"Barrier check 1: go signal: "<< go_signal_.load(std::memory_order_acquire)<< std::endl;
            
            } else {
                // Condition takes care of wrap around.
                while(go_signal_.load(std::memory_order_acquire) < current_seq 
                 && ((current_seq -go_signal_) < count )) {
                    //  std::cout <<"Barrier check 2\n"<<std::endl;
                    //  std::cout <<"Go Signal: "<<go_signal_.load(std::memory_order_acquire)<< ", current_seq: "<<current_seq  <<std::endl;
                }
                //  std::cout <<"Barrier check 2.2\n"<<std::endl;
            }
            return WAIT_STATUS::SUCCESS;
        }

    private:
        std::atomic<int> go_signal_;
        std::atomic<int> current_;
};

//---------------------------------------------------------------------------
	class ScopedBarrier{
	public:
		ScopedBarrier(std::atomic<bool>& sync_flag) :sync_flag_(sync_flag)
		{
			bool expected = false;
			while (!sync_flag_.compare_exchange_weak(expected, true, std::memory_order_acq_rel) ) {
				expected = false;
			}
	
		}

		~ScopedBarrier() {
			sync_flag_.store(false, std::memory_order_release);
		}
	private:
		std::atomic<bool>& sync_flag_;
	};
} //namespace barrier.