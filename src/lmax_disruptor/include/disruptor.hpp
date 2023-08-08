#pragma once

#include <atomic>
#include <new>
#include <stddef.h>
#include <utility>
#include <vector>
#include <array>
#include <iostream>

#define hardware_destructive_interference_size 128

namespace disruptor {

	//---------------------------------------------------------------------------
	// Alignment to  avoid false sharing.
	// The LMAX disruptor uses a ring buffer with each element occupying a cache line to 
	// prevent false sharing amongst threads.
	// In the tested mac OS, the cache line is 128 Bytes.
	// Additional information about the data such as whether it is the last in a stream is included.
	template <typename Elem>
	class alignas(hardware_destructive_interference_size) Sequence {
	public:
		bool	is_eof() const		{ return is_eof_;}
		void	set_eof(bool eof)	{ is_eof_ = eof;}
		Elem&	data()				{ return data_;}
	private:
		Elem	data_				{}; // Zero initialisation.
		bool	is_eof_				{false};	
	};

	//---------------------------------------------------------------------------
	// This class implements the ring buffer. 
	template<typename Elem, size_t N = 512>
	class RingBuffer {
		// Ensure size is a power of 2 and greater than 0.
		static_assert( N !=0 && ((N & (N - 1)) == 0));		

	public:
		class iterator {
		public:
			using 			_iter 										= typename std::array<Sequence<Elem>,N>::const_iterator;
			
			constexpr 		iterator() = default;
			constexpr 		iterator(_iter begin, size_t own_pos)		: begin_(std::move(begin)), own_pos_(own_pos) {}

			_iter 			get() const 								{ return begin_+ own_pos_; }
			iterator&		operator++()								{ own_pos_ = RingBuffer::GetBufferIDx(++own_pos_); return *this; }			
			Sequence<Elem> 	operator*() 								{ return *(begin_+own_pos_); }
			bool 			operator!=(const iterator& other )const 	{ return this->get() != other.get(); }
			bool 			operator==(const iterator& other )const 	{ return this->get() == other.get(); }

		private:
			_iter 			begin_;
			size_t 			own_pos_;
		};

		//---------------------------------------------------------------------------
		using 				SPtr 								= std::shared_ptr<RingBuffer>;

		constexpr			RingBuffer() 						= default;
		Sequence<Elem>& 	at(size_t pos)  					{ return buffer_[this->GetBufferIDx(pos)]; }
		static size_t 		GetBufferIDx (size_t sequence) 		{ return sequence & (N-1); }
		iterator 			GetIterator(size_t pos) const 		{ return iterator(buffer_.begin(), this->GetBufferIDx(pos)); }
		size_t 				size() const 						{ return N; }

	private:
		std::array<Sequence<Elem>,N> 	buffer_;
	};

	//---------------------------------------------------------------------------
	// Several update strategies for the disruptor's read/writer buffer pointers are investigated.
	// For slow reads or writes, other readers or writers respectively can be blocked waiting for others 
	// to catch up before they can resume reading or writing.
	// Rather than using a busy wait which blocks, one can buffer up update requests and move onto another read or write.
	// When the update request buffer is full then the reader or write will resort to busy waits.
	enum PublishPolicy{BUFFERED = 0, BLOCK};

	//---------------------------------------------------------------------------
	struct ReservationInfo {
		size_t 		pos_begin{};
		size_t 		pos_end{};
		bool 		err{true};
	};

	namespace detail 
	{
		//---------------------------------------------------------------------------

		enum PublishUpdateStatus{SUCCESS, ERROR, NO_SPACE};

		//---------------------------------------------------------------------------
		struct Reservation {

			Reservation(size_t begin, size_t end)		: pos_begin(begin), pos_end(end), is_initialised(true)	{}
			Reservation() 								= default;	

			size_t 		pos_begin{};
			size_t 		pos_end{};
			bool 		is_initialised{};
		};

		//---------------------------------------------------------------------------
		template< PublishPolicy>
		class CursorUpdateHelper;

		template<PublishPolicy P>
		std::ostream& 	operator<< (std::ostream&, const CursorUpdateHelper<P>&);

		//---------------------------------------------------------------------------
		template<>
		class CursorUpdateHelper<PublishPolicy::BUFFERED> {
			static constexpr 			size_t 			LIM = 20;
		public:
			// Subscribers must not be greater than 200.
			constexpr				CursorUpdateHelper() 								= default;
									CursorUpdateHelper(const std::string& type)			:type_(type)	{}

			// Thread-safe.
			PublishUpdateStatus 	UpdateCursor(const size_t pos_begin, const size_t pos_end);

			size_t 					cursor() const 		{ return cursor_.load(std::memory_order_acquire); }

			void 					Reset() 			{ cursor_ = 0;	std::fill( unprocessed_reservations_.begin(), unprocessed_reservations_.end(), Reservation{} ); }
			
			friend std::ostream& 	operator<< <>(std::ostream&, const CursorUpdateHelper&);
		private:
			// Should be at least the size of writers.
			std::array<Reservation, LIM> 	unprocessed_reservations_;
			std::string						type_{};
			std::atomic<size_t> 			cursor_{};
			std::atomic<bool >				sync_flag_{};
			
		};


		//---------------------------------------------------------------------------
		template<>
		class CursorUpdateHelper<PublishPolicy::BLOCK>  {
		public:
			constexpr 				CursorUpdateHelper() = default;
					 				CursorUpdateHelper(const std::string& type): type_(type){}

			// Thread-safe.
			PublishUpdateStatus 	UpdateCursor(const size_t pos_begin, const size_t pos_end);

			size_t 					cursor() const 			{ return cursor_.load(std::memory_order_acquire); }

			void 					Reset() 				{ cursor_ = 0; }
			
			friend std::ostream& operator<< <>(std::ostream&, const CursorUpdateHelper&);
		private:
			// Should be at least the size of writers.
			std::string				type_{};
			std::atomic<size_t> 	cursor_{};
		};
	}// detail

//---------------------------------------------------------------------------
template <class Derived, typename Elem, PublishPolicy P>
class Cursor{
public:
	using SPtr = std::shared_ptr<Cursor>;
	constexpr			Cursor
						(	typename RingBuffer<Elem>::SPtr 	buffer, 
							const std::string& 					type_in
						)
							:  
							buffer_								(std::move(buffer)),
							type_								(type_in), 
							cursor_updater_						(detail::CursorUpdateHelper<P>(type_)) 
						{}
	size_t 				GetCursor() const 						{ return cursor_updater_.cursor();}
	void 				Publish(size_t pos_begin, size_t pos_end);

	template <typename Derived2, PublishPolicy P2>
	ReservationInfo  	Reserve(const Cursor<Derived2, Elem, P2>& other_cursor,	size_t no_of_slots=1) {	static_cast<Derived*>(this)->Reserve(other_cursor, no_of_slots); }

	void 				Reset() 								{ this->cursor_updater_.Reset();	this->claim_sequence_.store(0); }
protected:
	typename RingBuffer<Elem>::SPtr 	buffer_;
	std::string 						type_{};
	detail::CursorUpdateHelper<P>		cursor_updater_{};
	std::atomic<size_t> 				claim_sequence_{};

};

//---------------------------------------------------------------------------
template <typename Elem, PublishPolicy _WP>
class WriteCursor: public Cursor<WriteCursor<Elem, _WP>, Elem, _WP >{
public:
	using 				SPtr 													= std::shared_ptr<WriteCursor>;

	constexpr 			WriteCursor(typename RingBuffer<Elem>::SPtr buffer)		: Cursor<WriteCursor, Elem, _WP>(std::move(buffer), "Writer"){}

	template <typename Derived, PublishPolicy P2>
	ReservationInfo  	Reserve(const Cursor<Derived, Elem, P2>&,	size_t no_of_slots=1);

	// Not thread-safe. Assumes we use this safely by reserving a space.
	void 				Write(size_t slot, Elem&&data, bool is_eof);
};

//---------------------------------------------------------------------------
template <typename Elem, PublishPolicy>
class ReadCursor;

template <typename Elem, PublishPolicy P>
class ReadResult {	
public:

	using _BufferIter = typename RingBuffer<Elem>::iterator;
	constexpr ReadResult() = default;
	constexpr ReadResult(	_BufferIter 			begin_in, 
							_BufferIter 			end_in, 
							bool 					err_in, 
							size_t 					start, 
							size_t 					end, 
							ReadCursor<Elem, P>* 	read_cursor)
							:  
							begin					(std::move(begin_in)), 
							end						(std::move(end_in)), 
							err						(err_in), 
							start_					(start), 
							end_					(end), 
							read_cursor_			(read_cursor) 
							{}

	_BufferIter 			begin;
	// As with standard convention, end is not part of the data to be read.
	_BufferIter 			end;
	bool 					err {true}; // Default must be true. Don't change.
	void 					Release() {if (read_cursor_) { read_cursor_->Publish(start_, end_);}}
private:
	size_t 					start_;
	size_t 					end_;
	ReadCursor<Elem, P>* 	read_cursor_;	
};

//---------------------------------------------------------------------------
template <typename Elem, PublishPolicy P>
class ReadCursor: public Cursor<ReadCursor<Elem, P>, Elem, P>{
public:
	constexpr 			ReadCursor( 	typename RingBuffer<Elem>::SPtr 	buffer )
										:
										Cursor<ReadCursor, Elem, P>			(std::move(buffer), "Reader") 
										{}

	template <typename Derived, PublishPolicy P2>
	ReservationInfo  	Reserve( const Cursor<Derived, Elem, P2 >&, size_t no_of_slots = 1);
	
	void 				Publish( size_t pos_begin, size_t pos_end );
	// Not thread-safe. Assumes we use this safely by reserving a space.
	ReadResult<Elem, P> Read( size_t slot_begin, size_t slot_end );
};

//---------------------------------------------------------------------------
template <typename Elem, PublishPolicy _WP, PublishPolicy _RP>
class ReaderWriter{
public:
	using SPtr = std::shared_ptr<ReaderWriter>;

	constexpr				ReaderWriter (	typename RingBuffer<Elem>::SPtr buffer )
									:
									write_cursor_		(WriteCursor<Elem, _WP>(buffer)),
									read_cursor_		(ReadCursor<Elem, _RP>(buffer)) 
									{}

	bool 					Write( Elem&& data, bool is_eof );

	// Returns at most num values to be read.
	ReadResult<Elem, _RP> 	Read(size_t num=1);

	void 					Reset();
	size_t 					GetWriteCursor() const	{ return write_cursor_.GetCursor(); }


private:
	WriteCursor<Elem, _WP>	write_cursor_;
	ReadCursor<Elem, _RP> 	read_cursor_;

};

//---------------------------------------------------------------------------
template <typename Elem, PublishPolicy _WP, PublishPolicy _RP>
class Writer {
	using 				_ReadWriterSPtr 						= typename ReaderWriter<Elem,  _WP, _RP>::SPtr;

public:
	constexpr			Writer(_ReadWriterSPtr  reader_writer)	: reader_writer_(std::move(reader_writer)){}

	// Blocking write with universal forwarding.
	bool 				Write(Elem&& data, bool is_eof=false) 	{ return reader_writer_->Write(std::forward<Elem>(data),is_eof); }
	size_t 				GetCursor() const 						{ return reader_writer_->GetWriteCursor();}
private:
	_ReadWriterSPtr 	reader_writer_;
};

//---------------------------------------------------------------------------
template <typename Elem, PublishPolicy _WP, PublishPolicy _RP>
class Reader {
	using 					_ReadWriterSPtr 							= typename ReaderWriter<Elem,  _WP, _RP>::SPtr;
public:
	constexpr				Reader (_ReadWriterSPtr reader_writer)		:reader_writer_(std::move(reader_writer)){}

	ReadResult<Elem, _RP> 	Read(size_t slot)							{ return reader_writer_->Read(slot); }
private:
	_ReadWriterSPtr 		reader_writer_;
};

//---------------------------------------------------------------------------
template <typename Elem, PublishPolicy _WP, PublishPolicy _RP>
class SingleDisruptor{
	using 				_ReaderWriterT		= typename ReaderWriter<Elem, _WP, _RP>::SPtr;
	using 				_RingBufferT 		= typename RingBuffer<Elem>::SPtr;
public:
	constexpr 			SingleDisruptor(	_ReaderWriterT 		reader_writer, 
											_RingBufferT 		buffer)
											:
											reader_writer_		( std::move(reader_writer)), 
											buffer_				( std::move(buffer)) 
											{}


	Writer<Elem, _WP, _RP> 		CreateWriter ();
	Reader<Elem, _WP, _RP> 		CreateReader ();

	void 						ResetReaderWriter();
	_RingBufferT  				buffer() 		{ return buffer_; }

	SingleDisruptor(){}
private:
	_ReaderWriterT					reader_writer_;
	typename RingBuffer<Elem>::SPtr buffer_;
};

//---------------------------------------------------------------------------
template <typename Elem, PublishPolicy _WP=PublishPolicy::BUFFERED, PublishPolicy _RP=PublishPolicy::BUFFERED>
 SingleDisruptor<Elem, _WP, _RP> MakeSingleDisruptor();

} //disruptor
#include "disruptor.ipp"
