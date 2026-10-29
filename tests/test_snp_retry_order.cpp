// REVATE (2026-10-29): SNP_FindRetryInsertBefore (src/steamnetworkingsockets/clientlib/
// steamnetworkingsockets_snp_retry_order.h) must pick EXACTLY the node upstream's forward scan
// picked -- retries stay in order within a lane, whatever hint it is given -- and the timeout
// shapes seen on a Miner's Haven soak server must cost O(1) per segment, not O(queue): an in-order
// wave (goes at the tail; the forward scan spent 96% of the GNS service thread in
// SNP_QueueReliableSegmentsForRetry and froze every peer), resends going back near the head (a
// tail-only walk: 98%) and a run of resends going back into the MIDDLE (both ends: 31%).
//
// Standalone: g++ -std=c++17 -O1 -I src/steamnetworkingsockets/clientlib tests/test_snp_retry_order.cpp
// (a stub list with CUtlLinkedList's handle API, so no tier0 link is needed). Exit 0 = pass.
#include "steamnetworkingsockets_snp_retry_order.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <random>

using namespace SteamNetworkingSocketsLib;

// The subset of CUtlLinkedList<uint16,uint16> the helper and the forward scan use.
struct List
{
	typedef uint16_t IndexType_t;
	struct Node { uint16_t prev, next, val; bool used; };
	std::vector<Node> n; uint16_t head = 0xFFFF, tail = 0xFFFF;
	uint16_t InvalidIndex() const { return 0xFFFF; }
	uint16_t Head() const { return head; }
	uint16_t Tail() const { return tail; }
	uint16_t Next( uint16_t h ) const { return n[h].next; }
	uint16_t Previous( uint16_t h ) const { return n[h].prev; }
	bool IsInList( uint16_t h ) const { return h < n.size() && n[h].used; }
	uint16_t operator[]( uint16_t h ) const { return n[h].val; }
	uint16_t InsertBefore( uint16_t before, uint16_t val )
	{
		uint16_t h = (uint16_t)n.size(); n.push_back( { 0xFFFF, 0xFFFF, val, true } );
		if ( before == 0xFFFF ) { n[h].prev = tail; if ( tail != 0xFFFF ) n[tail].next = h; else head = h; tail = h; }
		else { uint16_t p = n[before].prev; n[h].prev = p; n[h].next = before; n[before].prev = h;
		       if ( p != 0xFFFF ) n[p].next = h; else head = h; }
		return h;
	}
	void Remove( uint16_t h )
	{
		uint16_t p = n[h].prev, x = n[h].next;
		if ( p != 0xFFFF ) n[p].next = x; else head = x;
		if ( x != 0xFFFF ) n[x].prev = p; else tail = p;
		n[h].used = false;
	}
};

struct Seg { int lane; int64_t begin; };

// Upstream's forward scan, verbatim in shape (GNS v1.6.1 SNP_QueueReliableSegmentsForRetry).
static uint16_t ForwardScan( const List &l, const std::vector<Seg> &segs, int lane, int64_t begin, int *pSteps )
{
	int steps = 0; uint16_t h = l.Head();
	while ( h != l.InvalidIndex() )
	{
		++steps;
		const Seg &s = segs[ l[h] ];
		if ( s.lane == lane && s.begin > begin ) break;
		h = l.Next( h );
	}
	*pSteps = steps; return h;
}

static int g_fail = 0;
#define CHECK( c, ... ) do { if ( !(c) ) { std::printf( "FAIL %s:%d: ", __FILE__, __LINE__ ); std::printf( __VA_ARGS__ ); std::printf( "\n" ); ++g_fail; } } while ( 0 )

static bool LanesSorted( const List &l, const std::vector<Seg> &segs )
{
	int64_t last[8]; for ( auto &x : last ) x = INT64_MIN;
	for ( uint16_t h = l.Head(); h != l.InvalidIndex(); h = l.Next( h ) )
	{
		const Seg &s = segs[ l[h] ];
		if ( s.begin < last[ s.lane ] ) return false;
		last[ s.lane ] = s.begin;
	}
	return true;
}

int main()
{
	// 1. Same insertion node as the forward scan, on random lanes, random order and removals.
	std::mt19937 rng( 20261029 );
	int compared = 0;
	for ( int trial = 0; trial < 400 && !g_fail; ++trial )
	{
		List l; std::vector<Seg> segs; std::vector<uint16_t> live; uint16_t hLast = 0xFFFF, hGone = 0xFFFF;
		const int nLanes = 1 + (int)( rng() % 4 );
		for ( int i = 0; i < 120; ++i )
		{
			if ( !live.empty() && rng() % 5 == 0 )   // a retry acked or sent: leaves from anywhere
			{
				size_t k = rng() % live.size(); hGone = live[k]; l.Remove( live[k] ); live.erase( live.begin() + k );
				continue;
			}
			Seg s{ (int)( rng() % nLanes ), (int64_t)( rng() % 1000 ) * 2 + (int64_t)i * 2001 % 7 };
			uint16_t idx = (uint16_t)segs.size(); segs.push_back( s );
			int fs = 0, bs = 0;
			uint16_t want = ForwardScan( l, segs, s.lane, s.begin, &fs );
			// the hint: the previous insert (as the engine passes it), any queued node, a removed one, or none
			const unsigned pick = rng() % 4;
			const uint16_t hHint = pick == 0 ? hLast : pick == 1 && !live.empty() ? live[ rng() % live.size() ] : pick == 2 ? hGone : (uint16_t)0xFFFF;
			uint16_t got = SNP_FindRetryInsertBefore( l, s.lane, s.begin,
				[&]( uint16_t h, int &lane, int64_t &b ) { lane = segs[ l[h] ].lane; b = segs[ l[h] ].begin; }, hHint, &bs );
			CHECK( got == want, "trial %d insert %d (hint kind %u): picked node %u, forward scan %u", trial, i, pick, got, want );
			++compared;
			hLast = l.InsertBefore( got, idx );
			live.push_back( hLast );
			CHECK( LanesSorted( l, segs ), "trial %d insert %d: a lane is out of order", trial, i );
		}
	}

	// 2. An in-order timeout wave (what a big unacked backlog nacked by packet order looks like):
	//    20,000 segments over 3 interleaved lanes. Forward scan ~N^2/2 node visits; the backward
	//    scan must stay O(1) per segment.
	{
		const int N = 20000; List l; std::vector<Seg> segs; segs.reserve( N );
		long long back = 0, fwd = 0; uint16_t hLast = 0xFFFF;
		for ( int i = 0; i < N; ++i )
		{
			Seg s{ i % 3, (int64_t)i * 40 };
			uint16_t idx = (uint16_t)segs.size(); segs.push_back( s );
			int fs = 0, bs = 0;
			if ( i % 997 == 0 ) { ForwardScan( l, segs, s.lane, s.begin, &fs ); fwd += (long long)fs; }
			uint16_t got = SNP_FindRetryInsertBefore( l, s.lane, s.begin,
				[&]( uint16_t h, int &lane, int64_t &b ) { lane = segs[ l[h] ].lane; b = segs[ l[h] ].begin; }, hLast, &bs );
			CHECK( got == l.InvalidIndex(), "in-order segment %d not appended", i );
			back += bs;
			hLast = l.InsertBefore( got, idx );
		}
		CHECK( back <= 7LL * N, "in-order wave visited %lld nodes for %d segments (want <= %d)", back, N, 7 * N );
		std::printf( "in-order wave: %d segments, lock-step walk %lld node visits (%.2f/segment); "
		             "forward scan sampled at every 997th insert averaged %.0f visits/segment\n",
		             N, back, (double)back / N, (double)fwd / ( N / 997 + 1 ) );
	}

	// 3. A retransmit times out again: 20,000 newer retries queued, then 100 old segments (smaller
	//    begins than everything queued, in order) go back near the head. A tail-only walk crosses
	//    the whole queue for each; the two-ended walk must stay near the head.
	{
		const int N = 20000, K = 100; List l; std::vector<Seg> segs; segs.reserve( N + K );
		long long walked = 0; uint16_t hLast = 0xFFFF;
		auto put = [&]( int64_t begin, bool count )
		{
			Seg s{ 0, begin }; uint16_t idx = (uint16_t)segs.size(); segs.push_back( s );
			int fs = 0, bs = 0;
			uint16_t want = ForwardScan( l, segs, 0, begin, &fs );
			uint16_t got = SNP_FindRetryInsertBefore( l, 0, begin,
				[&]( uint16_t h, int &ln, int64_t &b ) { ln = segs[ l[h] ].lane; b = segs[ l[h] ].begin; }, hLast, &bs );
			CHECK( got == want, "begin %lld: picked %u, forward scan %u", (long long)begin, got, want );
			if ( count ) walked += bs;
			hLast = l.InsertBefore( got, idx );
		};
		for ( int i = 0; i < N; ++i ) put( 100000 + (int64_t)i * 40, false );
		for ( int i = 0; i < K; ++i ) put( 10 + (int64_t)i * 40, true );
		const long long want = 3LL * ( (long long)K * ( K + 1 ) / 2 + K );
		CHECK( walked <= want, "re-queued retransmit: %d old segments walked %lld nodes (want <= %lld)", K, walked, want );
		std::printf( "re-queued retransmit: %d old segments behind %d newer ones walked %lld nodes "
		             "(a tail-only walk: ~%lld)\n", K, N, walked, (long long)K * N );
	}

	// 4. A run of resends goes back into the MIDDLE: 20,000 queued 1,000 apart, then 200 neighbours
	//    that all belong in one gap halfway down. Both ends are ~10,000 away; next to the previous
	//    insert is O(1).
	{
		const int N = 20000, K = 200; List l; std::vector<Seg> segs; segs.reserve( N + K );
		long long walked = 0; uint16_t hLast = 0xFFFF;
		auto put = [&]( int64_t begin, bool count )
		{
			Seg s{ 0, begin }; uint16_t idx = (uint16_t)segs.size(); segs.push_back( s );
			int fs = 0, bs = 0;
			uint16_t want = ForwardScan( l, segs, 0, begin, &fs );
			uint16_t got = SNP_FindRetryInsertBefore( l, 0, begin,
				[&]( uint16_t h, int &ln, int64_t &b ) { ln = segs[ l[h] ].lane; b = segs[ l[h] ].begin; }, hLast, &bs );
			CHECK( got == want, "begin %lld: picked %u, forward scan %u", (long long)begin, got, want );
			if ( count ) walked += bs;
			hLast = l.InsertBefore( got, idx );
		};
		for ( int i = 0; i < N; ++i ) put( (int64_t)i * 1000, false );
		hLast = 0xFFFF;   // the run starts cold: its first insert walks from the ends
		for ( int i = 0; i < K; ++i ) put( (int64_t)( N / 2 ) * 1000 + 1 + i, true );
		CHECK( walked <= 3LL * N + 10LL * K, "mid-queue run: %d neighbours walked %lld nodes (want <= %lld)", K, walked, 3LL * N + 10LL * K );
		std::printf( "mid-queue run: %d neighbours %d deep walked %lld nodes (both ends only: ~%lld)\n", K, N / 2, walked, 2LL * K * ( N / 2 ) );
	}

	std::printf( "%s: %d insert points compared against the forward scan\n", g_fail ? "FAIL" : "PASS", compared );
	return g_fail ? 1 : 0;
}
