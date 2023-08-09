#define CATCH_CONFIG_MAIN
#include "disruptor.hpp"

#include <array>
#include <catch2/catch.hpp>

#include <future>
#include <iostream>
#include <numeric>
#include <limits>

#include "barrier.hpp"
#include "scoped_profiler.hpp"

#include "test_helpers.hpp"

TEST_CASE("TEST BUFFERED CURSOR HELPER SEQUENTIALLY") {
	// When cursor update requests are added
	// such that number of requests is equal or less than the wait internal buffer (~20), 
	// then we expect that the cursor eventually gets updated to the latest.
	disruptor::detail::CursorUpdateHelper<disruptor::PublishPolicy::BUFFERED> helper;
	constexpr size_t NoOfWriters = 20;
		size_t prev_i = 0;
		for (size_t i = 1; i < NoOfWriters; ++i) {
			helper.UpdateCursor(prev_i,i);
			prev_i = i;
		}

	REQUIRE(helper.cursor() == NoOfWriters-1);
}

TEST_CASE("TEST BUFFERED CURSOR HELPER IN PARALLEL") {
	// When cursor update requests are added in parallel 
	// such that number of requests is equal or less than the wait internal buffer (~20), 
	// then we expect that the cursor eventually gets updated to the latest.
	disruptor::detail::CursorUpdateHelper<disruptor::PublishPolicy::BUFFERED> helper;
	constexpr size_t NoOfWriters = 20;
	
	std::array<std::future<disruptor::detail::PublishUpdateStatus>, NoOfWriters> futures{};

	for (size_t i = NoOfWriters; i > 0; --i) {

		auto fn  = [&helper] (size_t prev_i, size_t i) {
			return helper.UpdateCursor(prev_i, i);
		};
		futures[i-1] = std::async(std::launch::async, fn, i-1,i);
	}

	for (size_t i = 1; i < NoOfWriters; ++i) {
		if (futures[i].valid())
			REQUIRE(futures[i].get() != disruptor::detail::PublishUpdateStatus::ERROR);
	}

	auto fn = [&]() {
		return helper.cursor() == NoOfWriters;
	};
	REQUIRE(tests::WAIT_TEST(fn));

}

SCENARIO( "Writer Performance Tests") {
	// When we use writers to write to a buffer in parallel without reading. 
	// As long as the number of writes are under the ring buffer capacity (~512),
	// We expect the write cursor to be equal to the number of writers.
	GIVEN( "A set of writers " ) {
		constexpr size_t NoOfWriters = 5;
		auto disruptor = disruptor::MakeSingleDisruptor<size_t>();
		using DisruptorType = disruptor::SingleDisruptor<size_t, disruptor::PublishPolicy::BUFFERED, disruptor::PublishPolicy::BUFFERED>;
		using WriterType = disruptor::Writer<size_t, disruptor::PublishPolicy::BUFFERED, disruptor::PublishPolicy::BUFFERED>;

		std::vector<WriterType> writers;
		writers.reserve(NoOfWriters);
		for (size_t i = 0; i < NoOfWriters; ++i) 
		{
			writers.push_back(disruptor.CreateWriter());
		}

		WHEN ("Writer write unique set of data without a reader reading") {
			constexpr size_t NoOfWritesPerWriter = 100;
			barrier::Barrier <NoOfWriters>b;

			auto callback = std::bind (&DisruptorType::ResetReaderWriter, disruptor);
			auto loop = [&] (WriterType& writer, const size_t data) {
				
				size_t data_t = data;

				for (size_t i = 0; i < NoOfWritesPerWriter; ++i) {	
					b.Wait(callback);
					auto d = data_t;
					writer.Write(std::forward<size_t>(d));
					++data_t;
					
				}
			};

			std::array<std::future<void>, NoOfWriters> futures{};

			size_t start_count  = 0;
			for (size_t i = 0; i < NoOfWriters; ++i) {
				futures[i] = std::async(std::launch::async, loop, std::ref(writers[i]), start_count);
				start_count += NoOfWritesPerWriter;
			}

			for (size_t i = 0; i< NoOfWriters; ++i) 
			{
				if(futures[i].valid()) 
					futures[i].wait();
			}

			THEN("Write Cursor must be correct") {
				auto fn = [&]() {
					std::cout << "write cursor: "<<writers[0].GetCursor() <<'\n';
					return writers[0].GetCursor()==NoOfWriters;
				};
				REQUIRE(tests::WAIT_TEST(fn));
			}
		}
	}
}

SCENARIO( "Reader Performance Tests") {
	// When we use writers to write to a buffer in parallel with a reading. 
	// we expect the reader to read all the values correctly. 
	// This is tested by ensuring all data written was read.
	GIVEN( "A set of writers " ) {
		
		constexpr size_t NoOfWriters = 3;

		using WriterType = disruptor::Writer<size_t, disruptor::PublishPolicy::BLOCK, disruptor::PublishPolicy::BUFFERED>;

		auto disruptor = disruptor::MakeSingleDisruptor<size_t, disruptor::PublishPolicy::BLOCK, disruptor::PublishPolicy::BUFFERED>();
		std::vector<WriterType>  writers;
		writers.reserve(NoOfWriters);

		for (size_t i = 0; i < NoOfWriters; ++i) 
		{
			writers.push_back(disruptor.CreateWriter());
		}
		
		WHEN ("Writer write unique set of data") {
			constexpr size_t NoOfWritesPerWriter = 5e5;
			auto loop = [&] (WriterType& writer, const size_t data) {
				
				size_t data_t = data;
				for (size_t i = 0; i < NoOfWritesPerWriter; ++i) {	
					size_t d = data_t;
					while(writer.Write(std::forward<size_t>(d))) {}					
					++data_t;
				}
			};

			std::array<std::future<void>, NoOfWriters> futures{};
			{
				size_t start_count  = 0;
				for (size_t i = 0; i < NoOfWriters; ++i) {
					futures[i] = std::async(std::launch::async, loop, std::ref(writers[i]), start_count);
					start_count += NoOfWritesPerWriter;
				}
			}
			AND_WHEN(" A single reader reads") {
				
				auto reader = disruptor.CreateReader();

				std::vector<size_t> sink; // Store for reads.
				sink.reserve(NoOfWriters*NoOfWritesPerWriter);
				profiler::Timer timer;		

				timer.Start();
				while (sink.size() < NoOfWriters*NoOfWritesPerWriter) 
				{
					auto read_result = reader.Read(128);
					if (read_result.err) { continue;}
					for (auto iter = read_result.begin; iter != read_result.end; ++iter) 
					{
						sink.push_back((*iter).data());
					}
					read_result.Release();
				}
				timer.Stop();


				THEN("All data must be read and in timely manner. ") {
					for (size_t i = 0; i < NoOfWriters; ++i) 
					{
						if(futures[i].valid()) 
							futures[i].wait();
					}

					// Process time.
					timer.Display();

					// Verify uniqueness of data.
					REQUIRE(sink.size() == NoOfWriters*NoOfWritesPerWriter);

					// Let us investigate if all data has been read and if all read data is valid.
					// We require that each data is unique so we create hash table of values;
					// If any value appears more than once, 
					// then our unique assumption is incorrect and thus our system is faulty.
					std::array<size_t, NoOfWriters*NoOfWritesPerWriter> value_count{};

					for (size_t i=0; i< NoOfWriters*NoOfWritesPerWriter;++i) {
						const auto idx = sink[i];	
						++value_count[idx]; 
						REQUIRE(value_count[idx]==1);						
					}
				}
			}

		}
	
	}

}
TEST_CASE("TEST THAT DISRUPTOR IS FASTER THAN A SIMPLE THREADSAFE QUEUE") {
	constexpr size_t NoOfWriters = 3;
	constexpr size_t NoOfWritesPerWriter =200;

	auto disruptor_stats = tests::TimedDisruptorTask ( NoOfWriters,  NoOfWritesPerWriter);
	auto queue_stats = tests::TimedQueueTask ( NoOfWriters,  NoOfWritesPerWriter);

	std::cout << "Disruptor stats: " <<disruptor_stats <<'\n';
	std::cout << "Queue stats: " << queue_stats <<'\n';
	REQUIRE(disruptor_stats.mean < queue_stats.mean);
} 