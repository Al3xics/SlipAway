// Copyright Myceland Team, All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Templates/Function.h"
#include "ML_HexPathfinder.generated.h"

class AML_Tile;
class UML_HexGridSubsystem;

/**
 * Stateless hex-grid pathfinding utilities.
 * All methods are static — no instance needed.
 */
UCLASS()
class MYCELAND_API UML_HexPathfinder : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	/** Returns true if the tile can be walked on (Dirt or Grass and not blocked). */
	static bool IsTileWalkable(const AML_Tile* Tile);

	/**
	 * BFS on ONE board's axial grid. Fills OutAxialPath with the shortest path from StartAxial to GoalAxial
	 * whose every step passes CanStep (walkable + height). Returns true if a path was found.
	 * Used for moves that stay inside a board (recorded puzzle moves, hover preview, gamepad); moves across
	 * boards use FindPath on the global grid.
	 */
	static bool BuildPath_AxialBFS(const FIntPoint& StartAxial, const FIntPoint& GoalAxial,
	                               const TMap<FIntPoint, AML_Tile*>& GridMap,
	                               TArray<FIntPoint>& OutAxialPath);

	// ==================== Global grid movement ====================
	// Everything below works on the global hex grid (UML_HexGridSubsystem), across boards, with heights.
	// It is the same for puzzle and world tiles: the movement never asks what kind of board it is on,
	// except TruncateAtBoardEntry, which applies the "stop on the first tile of a puzzle" rule.

	/** Steps between two cells on the hex grid (0 for the same cell, 1 for neighbors). */
	static int32 HexDistance(const FIntPoint& A, const FIntPoint& B);

	/**
	 * Single rule for one step between NEIGHBOR tiles: To is walkable, the height difference is allowed
	 * (Project Settings > Myceland > Movement), and the board border can be crossed. Pathfinding, hover
	 * preview and gamepad must all go through it, so the glow and the real path never disagree.
	 */
	static bool CanStep(const AML_Tile* From, const AML_Tile* To);

	/**
	 * Hook for leaving / entering a board. Always true for now: leaving a puzzle needs no confirmation and
	 * every board can be entered on foot. The day a confirmation or a hard lock comes back, it goes here.
	 */
	static bool CanCrossBoardBorder(const AML_Tile* From, const AML_Tile* To) { return true; }

	/**
	 * A* on an abstract hex grid — no world, no tiles, so it can be unit-tested. CanStepFn says whether one
	 * step From -> To (neighbors) is allowed; a cell that does not exist simply answers false.
	 * Returns true when Goal is reached; OutPath then runs Start..Goal, both included.
	 * When Goal cannot be reached (or more than MaxExplored cells would be needed) and bAllowPartial is set,
	 * OutPath runs Start..the explored cell closest to Goal instead (only Start if nothing is closer), and
	 * the function still returns false. Without bAllowPartial, OutPath is left empty on failure.
	 */
	static bool FindPath_AStar(const FIntPoint& Start, const FIntPoint& Goal,
	                           TFunctionRef<bool(const FIntPoint& From, const FIntPoint& To)> CanStepFn,
	                           int32 MaxExplored, bool bAllowPartial, TArray<FIntPoint>& OutPath);

	/**
	 * A* between two tiles on the global grid, possibly on different boards, using CanStep. Same return
	 * value and partial behavior as FindPath_AStar; OutPath holds tiles, Start..end. Search size is capped
	 * by Pathfinding Max Explored Tiles in the developer settings.
	 */
	static bool FindPath(const UML_HexGridSubsystem& Grid, const AML_Tile* Start, const AML_Tile* Goal,
	                     bool bAllowPartial, TArray<AML_Tile*>& OutPath);

	/**
	 * Cuts Path right after its first step onto another board that stops the player (an open puzzle, see
	 * AML_BoardSpawner::ShouldStopPlayerOnEntry). Moves inside the board the path starts on are never cut,
	 * and walking out of a puzzle and back into it does stop on re-entry. Returns true if it cut anything.
	 */
	static bool TruncateAtBoardEntry(TArray<AML_Tile*>& Path);

	/**
	 * Pure core of TruncateAtBoardEntry, so the rule can be unit-tested without actors. StepBoards holds the
	 * board of each path step (any stable key, nullptr = no board); StopsOnEntry says whether stepping onto
	 * that board from another one must stop the player. Returns how many steps to keep (StepBoards.Num() when
	 * nothing is cut): everything up to and including the first step onto a stopping board.
	 */
	static int32 ComputeBoardEntryCut(TArrayView<const void* const> StepBoards, TFunctionRef<bool(const void* Board)> StopsOnEntry);
};
