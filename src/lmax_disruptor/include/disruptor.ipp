#include <iostream>
#include <stdlib.h>
#include <tuple>
#include <mutex>
#include <cassert>
#include <chrono>
#include <thread>

#include "barrier.hpp"

#define NDEBUG


namespace disruptor{

namespace {
	#ifndef NDEBUG
	std::mutex mprint;
	template <typename... Ts>
	void Print(const Ts&... ts) {
		
		std::scoped_lock lk (mprint);
		((std::cout << ts),...); 
	}
	#else
	#define Print(...) 
	#endif
}

namespace detail{

	//---------------------------------------------------------------------------
	std::mutex mout;
	template<>
	std::ostream& operator<<(std::ostream& os, const CursorUpdateHelper<PublishPolicy::BUFFERED>& c) 
	{
		std::scoped_lock lk(mout);
		os <<"//--------------CursorUpdateHelper---------------\n";
		os << "Cursor: " <<c.cursor_ ;
		os << ", Sync Flag: " <<c.sync_flag_;
		for (const auto& elem: c.unprocessed_reservations_) {
			os << " ( pos_begin:" << elem.pos_begin 
				<< ", pos_end: " << elem.pos_end
				<< ", is_initialised: " << elem.is_initialised <<") ";
		}
		os << "\n------------------------------------------\n";
		return os;
	}

	//---------------------------------------------------------------------------
	template <> 
	std::ostream& operator<< (std::ostream& os, const CursorUpdateHelper< PublishPolicy::BLOCK>& c) 
	{
		std::scoped_lock lk(mout);
		os <<"//--------------CursorUpdateHelper---------------\n";
		os << "Cursor: " <<c.cursor_ ;
		os << "\n------------------------------------------\n";
		return os;
	}

	//---------------------------------------------------------------------------
	PublishUpdateStatus CursorUpdateHelper<PublishPolicy::BUFFERED>::UpdateCursor(const size_t pos_begin, const size_t pos_end) 
	{
		
		PublishUpdateStatus err = PublishUpdateStatus::SUCCESS;
		assert(pos_begin<pos_end);

		barrier::ScopedBarrier b{sync_flag_};
		Print("Entering critical section\n");

		size_t c =  cursor_.load(std::memory_order_relaxed);

		if (pos_end <= c) 
		{
			Print("Error: cursor is same or greater then new end."
				,", cursor: ", c
				,", pos_begin:", pos_begin 
				,", pos_end: ", pos_end
				,'\n');	
			return  PublishUpdateStatus::ERROR;
		}

		if (pos_begin == c) 
		{
			// Find latest cursor. This can be optimised.
			size_t target = pos_end;
			while( std::find_if(unprocessed_reservations_.begin(), unprocessed_reservations_.end(), 
				[&](Reservation& reservation)
				{
					if (reservation.is_initialised && reservation.pos_begin == target) 
					{
						target = reservation.pos_end;
						reservation.is_initialised = false;
						return true;
					}
					return false;
				}) !=  unprocessed_reservations_.end())
				;

				assert(target>cursor_); 
	
				Print("Updating cursor"
					,", cursor: ", c
					,", new cursor: ", target
					,", pos_begin:", pos_begin 
					,", pos_end: ", pos_end
					,'\n');		
				cursor_= target;				
		} else 
		{
			// store sequence object in array.
			auto iter = std::find_if(unprocessed_reservations_.begin(),
									unprocessed_reservations_.end(), 
									[](const Reservation& reservation) 
										{
											return !reservation.is_initialised;	
										}
									);

			// if iter == unprocessed_reservations_.end() 
			// then reservation failed and writers or readers are not fast enough. In this case, retry reservation later.
			if (iter != unprocessed_reservations_.end()) 
			{
				*iter = {pos_begin, pos_end};
			} else 
			{
				err =  PublishUpdateStatus::NO_SPACE;
			}
		}

		return err;	
	}

	//---------------------------------------------------------------------------
	PublishUpdateStatus CursorUpdateHelper<PublishPolicy::BLOCK>::UpdateCursor(const size_t pos_begin, const size_t pos_end) 
	{
		size_t expected = pos_begin;
		while (!cursor_.compare_exchange_weak(expected, pos_end)) {
				expected = pos_begin;
		}
		return PublishUpdateStatus::SUCCESS;
	}
	

}// namespace detail

//---------------------------------------------------------------------------
template <typename Elem, PublishPolicy _WP>
template <class Derived, PublishPolicy P2>
ReservationInfo WriteCursor<Elem, _WP>::Reserve(const Cursor<Derived, Elem, P2>& read_cursor,
		size_t no_of_slots)
{
	size_t expected, new_sequence;
	bool err = false;

	auto wait_for_available_space = [&]() 
		{		
			expected = this->claim_sequence_.load(std::memory_order_acquire); 
			size_t read_cursor_seq = read_cursor.GetCursor();
			
			if (err = expected < read_cursor_seq; err ) 
			{
				Print("write claim seq: ", expected
						,", read cursor: ", read_cursor_seq
						, '\n');
				return true;
			}
		
		size_t claim_capacity =  this->buffer_->size() - (expected - read_cursor_seq);

		new_sequence = expected + std::min(claim_capacity, no_of_slots);
		return claim_capacity == 0;
	};
	
	// Available size can be zero and so we have to wait until slow reads are completed.
	while (wait_for_available_space()) {
		if (err) return {0, 0, true};
	}

	// Now there is space, update claim sequence.
	while (!this->claim_sequence_.compare_exchange_weak( expected, new_sequence )) 
	{
		while (wait_for_available_space()) {
			if (err) return {0, 0, true};
		}
	}
	
	assert(new_sequence > expected);
	return {expected, new_sequence, false}; 
}

//---------------------------------------------------------------------------	
template <typename Elem, PublishPolicy _WP>
void WriteCursor<Elem, _WP>::Write(size_t slot, Elem&&data, bool is_eof ) 
{
	// We assume no write contentions as slot is reserved.
	this->buffer_->at(slot).data() = data;

	if (is_eof) [[unlikely]] 
		this->buffer_->at(slot).set_eof(is_eof);
}

//---------------------------------------------------------------------------	
template <class Derived, typename Elem, PublishPolicy P>
void Cursor<Derived, Elem, P>::Publish(size_t pos_begin, size_t pos_end) 
{
	while (cursor_updater_.UpdateCursor(pos_begin, pos_end)==detail::PublishUpdateStatus::NO_SPACE) {
		Print(" Waiting to publish!"
			, "cursor type: ", this->type_
			, ", pos begin: ", pos_begin
			, ", pos end: ", pos_end 
			,'\n'
			, cursor_updater_ );			
	}
}

//---------------------------------------------------------------------------	
template <typename Elem, PublishPolicy _RP>
template <typename Derived, PublishPolicy P2>
ReservationInfo ReadCursor<Elem, _RP>::Reserve(const Cursor<Derived, Elem, P2>& write_cursor, size_t no_of_slots) 
{	
	size_t expected, new_sequence;
	size_t write_cursor_seq;
	

	// If nothing to read, then return.
	// Need to check for EoF and ensure non-blocking.
	auto is_no_available_data = [&] () {		
		
		expected = this->claim_sequence_.load(std::memory_order_acquire); 
		write_cursor_seq = write_cursor.GetCursor();
		assert(write_cursor_seq >= expected);
		size_t claim_capacity = write_cursor_seq - expected;

		new_sequence = expected	+ std::min(claim_capacity, no_of_slots);
		return claim_capacity==0;
	};
	
	// Available size can be zero and so we have to wait until slow reads are completed.
	if ( is_no_available_data() ) 
		return{ 0, 0, true};

	// Now there is space, update claim sequence.
	while (!this->claim_sequence_.compare_exchange_weak(expected, new_sequence)) 
	{
		if ( is_no_available_data() ) 
			return{ 0, 0, true};
	}	
	assert(write_cursor_seq >= new_sequence);
	assert(new_sequence > expected);
	return {expected, new_sequence, false}; 
}

//---------------------------------------------------------------------------
template <typename Elem, PublishPolicy _RP>
ReadResult<Elem, _RP> ReadCursor<Elem,_RP>::Read(size_t slot_begin, size_t slot_end) 
{
	return ReadResult<Elem, _RP>
	{
		this->buffer_->GetIterator(slot_begin), 
		this->buffer_->GetIterator(slot_end), 
		false,
		slot_begin,
		slot_end,
		this
	};
}

//---------------------------------------------------------------------------	
template <typename Elem, PublishPolicy _RP>
void ReadCursor<Elem, _RP>::Publish(size_t pos_begin, size_t pos_end) {
	Print("Attempting read publish: "
				, "pos_begin", pos_begin
				, "pos_end", pos_end
				, "Read cursor update helper: ", this->cursor_updater_);

	Cursor<ReadCursor<Elem, _RP>, Elem, _RP>::Publish (pos_begin, pos_end);
}

//---------------------------------------------------------------------------	
template <typename Elem, PublishPolicy _WP, PublishPolicy _RP>
	bool ReaderWriter<Elem, _WP, _RP>::Write(Elem&& data, bool is_eof) 
	{
		// Claim a space. Block if no space.
		ReservationInfo reservation = write_cursor_.Reserve(read_cursor_);
		
		if (reservation.err) [[unlikely]] 
			return true;

		write_cursor_.Write(reservation.pos_begin, std::forward<Elem>(data), is_eof);
		write_cursor_.Publish(reservation.pos_begin, reservation.pos_end);

		return false;
	}

//---------------------------------------------------------------------------	
template <typename Elem, PublishPolicy _WP, PublishPolicy _RP>
	ReadResult<Elem, _RP> ReaderWriter<Elem, _WP, _RP>::Read(size_t num) 
	{
		// Claim a space. Return err if no space.
		ReservationInfo reservation = read_cursor_.Reserve(write_cursor_, num);

		if (reservation.err) [[unlikely]] 
			return  {}; // Initialised to error state.
		
		return std::move (read_cursor_.Read(reservation.pos_begin, reservation.pos_end));
	}

//---------------------------------------------------------------------------	
template <typename Elem, PublishPolicy _WP, PublishPolicy _RP>
	void ReaderWriter<Elem, _WP, _RP>::Reset() 
	{
		this->read_cursor_.Reset();
		this->write_cursor_.Reset();
	}

//---------------------------------------------------------------------------	
template <typename Elem, PublishPolicy _WP, PublishPolicy _RP>
	Writer<Elem, _WP, _RP> SingleDisruptor<Elem, _WP, _RP>::CreateWriter () 
	{
		return Writer<Elem, _WP, _RP>(reader_writer_);
	}

//---------------------------------------------------------------------------	
template <typename Elem, PublishPolicy _WP, PublishPolicy _RP>
	Reader<Elem, _WP, _RP> SingleDisruptor<Elem, _WP, _RP>::CreateReader () 
	{
		return Reader<Elem, _WP, _RP>(reader_writer_);
	}

//---------------------------------------------------------------------------
template <typename Elem, PublishPolicy _WP, PublishPolicy _RP>
	void SingleDisruptor<Elem, _WP, _RP>::ResetReaderWriter() 
	{
		this->reader_writer_->Reset();
	}

//---------------------------------------------------------------------------
template <typename Elem, PublishPolicy _WP, PublishPolicy _RP>
 	SingleDisruptor<Elem, _WP, _RP> MakeSingleDisruptor() 
	{
		auto buffer = std::make_shared<RingBuffer<Elem>>();

		auto reader_writer = std::make_shared<ReaderWriter<Elem, _WP, _RP>>(buffer);
		SingleDisruptor<Elem, _WP, _RP> disruptor(std::move(reader_writer), std::move(buffer));

		return disruptor;
	}

} // Namespace disruptor
