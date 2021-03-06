/*
    Laser, a UCI chess engine written in C++11.
    Copyright 2015-2018 Jeffrey An and Michael An

    Laser is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Laser is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Laser.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "search.h"
#include "moveorder.h"

const int SCORE_IID_MOVE = (1 << 20);
const int SCORE_WINNING_CAPTURE = (1 << 18);
const int SCORE_QUEEN_PROMO = (1 << 17);
const int SCORE_EVEN_CAPTURE = (1 << 16);
const int SCORE_QUIET_MOVE = -(1 << 30);
const int SCORE_LOSING_CAPTURE = -(1 << 30) - (1 << 28);


MoveOrder::MoveOrder(Board *_b, int _color, int _depth, bool _isPVNode,
	SearchParameters *_searchParams, SearchStackInfo *_ssi, Move _hashed, MoveList _legalMoves) {
	b = _b;
	color = _color;
	depth = _depth;
	isPVNode = _isPVNode;
	searchParams = _searchParams;
    ssi = _ssi;
    mgStage = STAGE_NONE;
    quietStart = 0;
    index = 0;
    hashed = _hashed;
    legalMoves = _legalMoves;
}

// Returns true if there are still moves remaining, false if we have
// generated all moves already
void MoveOrder::generateMoves() {
    switch (mgStage) {
        // The hash move, if any, is handled separately from the rest of the list
        case STAGE_NONE:
            if (hashed != NULL_MOVE) {
                mgStage = STAGE_HASH_MOVE;

                // Remove the hash move from the list, since it has already been tried
                for (unsigned int i = 0; i < legalMoves.size(); i++) {
                    if (legalMoves.get(i) == hashed) {
                        legalMoves.remove(i);
                        break;
                    }
                }

                break;
            }
            // else fallthrough

        // If we just searched the hash move (or there is none), we need to find
        // where the quiet moves start in the list, and then score captures.
        case STAGE_HASH_MOVE:
            findQuietStart();
            mgStage = STAGE_CAPTURES;
            scoreCaptures();
            break;

        // After winnning captures, we score quiets
        case STAGE_CAPTURES:
            mgStage = STAGE_QUIETS;
            scoreQuiets();
            break;

        // We are done
        case STAGE_QUIETS:
            break;
    }
}

// Sort captures using SEE and MVV/LVA
void MoveOrder::scoreCaptures() {
    for (unsigned int i = 0; i < quietStart; i++) {
        Move m = legalMoves.get(i);

        // We want the best move first for PV nodes
        if (isPVNode) {
            int see = b->getSEEForMove(color, m);

            if (see > 0)
                scores.add(SCORE_WINNING_CAPTURE + see + b->getMVVLVAScore(color, m));
            else if (see == 0)
                scores.add(SCORE_EVEN_CAPTURE + b->getMVVLVAScore(color, m));
            else
                // If we are doing SEE on quiets, score losing captures lower
                scores.add(SCORE_LOSING_CAPTURE + see + b->getMVVLVAScore(color, m));
        }

        // Otherwise, MVV/LVA for cheaper cutoffs might help
        else {
            // Use exchange score to save an SEE if possible: if the SEE is
            // winning for us on the first turn, then we can stand pat after
            // our opponent's recapture
            int exchange = b->getExchangeScore(color, m);

            if (exchange > 0)
                scores.add(SCORE_WINNING_CAPTURE + b->getMVVLVAScore(color, m));

            else if (exchange == 0)
                scores.add(SCORE_EVEN_CAPTURE + b->getMVVLVAScore(color, m));

            // If the initial capture is losing, we need to check whether the
            // piece was hanging using SEE
            else {
                int see = b->getSEEForMove(color, m);

                if (see > 0)
                    scores.add(SCORE_WINNING_CAPTURE + b->getMVVLVAScore(color, m));
                else if (see == 0)
                    scores.add(SCORE_EVEN_CAPTURE + b->getMVVLVAScore(color, m));
                else
                    scores.add(SCORE_LOSING_CAPTURE + b->getMVVLVAScore(color, m));
            }
        }
    }
}

void MoveOrder::scoreQuiets() {
    for (unsigned int i = quietStart; i < legalMoves.size(); i++) {
        Move m = legalMoves.get(i);

        // Score killers below even captures but above losing captures
        if (m == searchParams->killers[ssi->ply][0])
            scores.add(SCORE_EVEN_CAPTURE - 1);

        // Order queen promotions somewhat high
        else if (getPromotion(m) == QUEENS)
            scores.add(SCORE_QUEEN_PROMO);

        // Sort all other quiet moves by history
        else {
            int startSq = getStartSq(m);
            int endSq = getEndSq(m);
            int pieceID = b->getPieceOnSquare(color, startSq);

            scores.add(SCORE_QUIET_MOVE
                + searchParams->historyTable[color][pieceID][endSq]
                + ((ssi->counterMoveHistory != nullptr) ? ssi->counterMoveHistory[pieceID][endSq] : 0)
                + ((ssi->followupMoveHistory != nullptr) ? ssi->followupMoveHistory[pieceID][endSq] : 0));
        }
    }
}

// Retrieves the next move with the highest score, starting from index using a
// partial selection sort. This way, the entire list does not have to be sorted
// if an early cutoff occurs.
Move MoveOrder::nextMove() {
    // Special case when we have a hash move available
    if (mgStage == STAGE_HASH_MOVE)
        return hashed;

    // If we are the end of our generated list, generate more.
    // If there are no moves left, return NULL_MOVE to indicate so.
    while (index >= scores.size()) {
        if (mgStage == STAGE_QUIETS)
            return NULL_MOVE;
        else {
            generateMoves();
        }
    }

    // Find the index of the next best move
    int bestIndex = index;
    int bestScore = scores.get(index);
    for (unsigned int i = index + 1; i < scores.size(); i++) {
        if (scores.get(i) > bestScore) {
            bestIndex = i;
            bestScore = scores.get(bestIndex);
        }
    }

    // Swap the best move to the correct position
    legalMoves.swap(bestIndex, index);
    scores.swap(bestIndex, index);

    // Once we've gotten to even captures, we need to generate quiets since
    // some quiets (killers, promotions) should be searched first.
    if (mgStage == STAGE_CAPTURES && bestScore < SCORE_WINNING_CAPTURE)
        generateMoves();

    return legalMoves.get(index++);
}

// When a PV or cut move is found, the history of the best move in increased,
// and the histories of all quiet moves searched prior to the best move are reduced.
void MoveOrder::updateHistories(Move bestMove) {
    int histDepth = std::min(depth, 12);

    // Increase history for the best move
    int startSq = getStartSq(bestMove);
    int endSq = getEndSq(bestMove);
    int pieceID = b->getPieceOnSquare(color, startSq);
    searchParams->historyTable[color][pieceID][endSq] -=
        histDepth * searchParams->historyTable[color][pieceID][endSq] / 64;
    searchParams->historyTable[color][pieceID][endSq] += histDepth * histDepth;
    if (ssi->counterMoveHistory != nullptr) {
        ssi->counterMoveHistory[pieceID][endSq] -=
            histDepth * ssi->counterMoveHistory[pieceID][endSq] / 64;
        ssi->counterMoveHistory[pieceID][endSq] += histDepth * histDepth;
    }
    if (ssi->followupMoveHistory != nullptr) {
        ssi->followupMoveHistory[pieceID][endSq] -=
            histDepth * ssi->followupMoveHistory[pieceID][endSq] / 64;
        ssi->followupMoveHistory[pieceID][endSq] += histDepth * histDepth;
    }

    // If we searched only the hash move, return to prevent crashes
    if (index <= 0)
        return;
    for (unsigned int i = 0; i < index-1; i++) {
        if (legalMoves.get(i) == bestMove)
            break;
        if (isCapture(legalMoves.get(i)))
            continue;

        startSq = getStartSq(legalMoves.get(i));
        endSq = getEndSq(legalMoves.get(i));
        pieceID = b->getPieceOnSquare(color, startSq);

        searchParams->historyTable[color][pieceID][endSq] -=
            histDepth * searchParams->historyTable[color][pieceID][endSq] / 64;
        searchParams->historyTable[color][pieceID][endSq] -= histDepth * histDepth;
        if (ssi->counterMoveHistory != nullptr) {
            ssi->counterMoveHistory[pieceID][endSq] -=
                histDepth * ssi->counterMoveHistory[pieceID][endSq] / 64;
            ssi->counterMoveHistory[pieceID][endSq] -= histDepth * histDepth;
        }
        if (ssi->followupMoveHistory != nullptr) {
            ssi->followupMoveHistory[pieceID][endSq] -=
                histDepth * ssi->followupMoveHistory[pieceID][endSq] / 64;
            ssi->followupMoveHistory[pieceID][endSq] -= histDepth * histDepth;
        }
    }
}

void MoveOrder::findQuietStart() {
    for (unsigned int i = 0; i < legalMoves.size(); i++) {
        if (!isCapture(legalMoves.get(i))) {
            quietStart = i;
            return;
        }
    }

    // If there are no quiets
    quietStart = legalMoves.size();
}