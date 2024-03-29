#pragma once

#include <utility>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <type_traits>
#include <atomic>

#include "Common.h"

namespace util {
	template<typename T> class work_queue {
		static_assert(std::is_move_assignable<T>::value || std::is_move_constructible<T>::value, "typename T must be move assignable and constructible.");

		std::queue<T> items;
		std::mutex lock;
		std::condition_variable cv;
		std::atomic<bool> alive;

		public:
			class waiter_killed_exception {};

			work_queue(const work_queue& other) = delete;
			work_queue& operator=(const work_queue& other) = delete;

			exported work_queue() {
				this->alive = true;
			}

			exported ~work_queue() {
				this->kill_waiters();
			}

			exported work_queue(work_queue&& other) {
				*this = std::move(other);
			}

			exported work_queue& operator=(work_queue&& other) {
				std::unique_lock<std::mutex> lck1(this->lock);
				std::unique_lock<std::mutex> lck2(other.lock);

				this->items = std::move(other.items);

				return *this;
			}

			exported void enqueue(T&& item) {
				std::unique_lock<std::mutex> lock(this->lock);
				this->items.push(std::move(item));
				this->cv.notify_one();
			}

			exported bool dequeue(T& target) {
				if (!this->alive)
					return false;

				std::unique_lock<std::mutex> lock(this->lock);

				while (this->items.empty()) {
					this->cv.wait(lock);

					if (!this->alive)
						return false;
				}

				target = std::move(this->items.front());
				this->items.pop();

				return true;
			}

			exported T dequeue() {
				if (!this->alive)
					throw waiter_killed_exception();

				std::unique_lock<std::mutex> lock(this->lock);

				while (this->items.empty()) {
					this->cv.wait(lock);

					if (!this->alive)
						throw waiter_killed_exception();
				}

				T request(std::move(this->items.front()));
				this->items.pop();

				return std::move(request);
			}

			exported void kill_waiters() {
				this->alive = false;
				this->cv.notify_all();
			}
	};
}
