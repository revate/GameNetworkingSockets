//====== Copyright Valve Corporation, All rights reserved. ====================
//
// REVATE (2026-10-29): where a reliable segment joins the ready-to-retry queue.
// Split out of SNP_QueueReliableSegmentsForRetry so it can be tested on its own
// (tests/test_snp_retry_order.cpp) against upstream's forward scan.
//
//=============================================================================

#ifndef STEAMNETWORKINGSOCKETS_SNP_RETRY_ORDER_H
#define STEAMNETWORKINGSOCKETS_SNP_RETRY_ORDER_H
#pragma once

namespace SteamNetworkingSocketsLib {

/// The node the new segment must be inserted BEFORE (InvalidIndex() = append):
/// the first entry, in list order, of the same lane whose begin is greater.
///
/// Within a lane the queue is sorted ascending by begin (this choice is what
/// keeps it so), so the answer can be found from anywhere: forward, it is the
/// first same-lane entry with a greater begin; backward, it is the last greater
/// one seen before a same-lane entry that is not greater. Three walks run in
/// lock step and the first to finish answers: from the head, from the tail, and
/// from `hHint` (the caller's previous insert, if still queued). The cost is
/// about three times the distance from the NEAREST of the three.
///
/// Upstream walked forward from the head only: O(queue) per segment, O(n^2) per
/// timeout wave, under the connection lock, on the one service thread for every
/// peer (a Miner's Haven soak server spent 96% of that thread here and froze
/// every peer for up to 25 s). The ends alone are not enough: retries whose
/// resend timed out go back into the MIDDLE of a long queue (measured: 31% of the
/// thread still here, every peer timed out). They arrive as runs of neighbours,
/// so each lands next to the previous insert: the hint walk is O(1) for them.
///
/// `laneBegin(h, lane, begin)` reads the lane and begin of queue node h.
/// `hHint`: a node handle or InvalidIndex(); used only when IsInList(hHint).
/// `pSteps`, when given, gets the number of nodes visited (for the test).
template <typename TList, typename TLaneBegin, typename TBegin>
inline typename TList::IndexType_t SNP_FindRetryInsertBefore( const TList &list, int nLane, TBegin nBegin,
	TLaneBegin laneBegin, typename TList::IndexType_t hHint = (typename TList::IndexType_t)~0u, int *pSteps = nullptr )
{
	typedef typename TList::IndexType_t H;
	const H hInvalid = list.InvalidIndex();
	int nSteps = 0;
	int nLaneH; TBegin nBeginH;

	// The hint walk: forward from a smaller same-lane node, or backward from a greater one.
	H hHintCur = hInvalid, hHintCandidate = hInvalid;
	bool bHint = false, bHintForward = false;
	if ( hHint != hInvalid && list.IsInList( hHint ) )
	{
		++nSteps;
		laneBegin( hHint, nLaneH, nBeginH );
		if ( nLaneH == nLane )
		{
			bHint = true;
			bHintForward = !( nBeginH > nBegin );   // everything of this lane before it is not greater either
			if ( bHintForward )
				hHintCur = list.Next( hHint );
			else
			{
				hHintCandidate = hHint;              // everything of this lane after it is greater too
				hHintCur = list.Previous( hHint );
			}
		}
	}

	H hFwd = list.Head(), hBack = list.Tail(), hCandidate = hInvalid, hResult = hInvalid;
	for ( ;; )
	{
		// from the head
		if ( hFwd == hInvalid ) { hResult = hInvalid; break; }      // ran off the end: append
		++nSteps;
		laneBegin( hFwd, nLaneH, nBeginH );
		if ( nLaneH == nLane && nBeginH > nBegin ) { hResult = hFwd; break; }
		hFwd = list.Next( hFwd );

		// from the tail
		if ( hBack == hInvalid ) { hResult = hCandidate; break; }   // ran off the head
		++nSteps;
		laneBegin( hBack, nLaneH, nBeginH );
		if ( nLaneH == nLane )
		{
			if ( !( nBeginH > nBegin ) ) { hResult = hCandidate; break; }   // sorted lane: nothing earlier is greater
			hCandidate = hBack;   // an earlier one may still follow
		}
		hBack = list.Previous( hBack );

		// from the hint
		if ( bHint )
		{
			if ( bHintForward )
			{
				if ( hHintCur == hInvalid ) { hResult = hInvalid; break; }
				++nSteps;
				laneBegin( hHintCur, nLaneH, nBeginH );
				if ( nLaneH == nLane && nBeginH > nBegin ) { hResult = hHintCur; break; }
				hHintCur = list.Next( hHintCur );
			}
			else
			{
				if ( hHintCur == hInvalid ) { hResult = hHintCandidate; break; }
				++nSteps;
				laneBegin( hHintCur, nLaneH, nBeginH );
				if ( nLaneH == nLane )
				{
					if ( !( nBeginH > nBegin ) ) { hResult = hHintCandidate; break; }
					hHintCandidate = hHintCur;
				}
				hHintCur = list.Previous( hHintCur );
			}
		}
	}
	if ( pSteps )
		*pSteps = nSteps;
	return hResult;
}

} // namespace SteamNetworkingSocketsLib

#endif // STEAMNETWORKINGSOCKETS_SNP_RETRY_ORDER_H
