// Copyright Myceland Team, All Rights Reserved.

#include "Player/ML_HexPathfinder.h"

#include "Algo/Reverse.h"
#include "Core/ML_CoreData.h"
#include "Tiles/ML_Tile.h"
#include "Core/ML_TileTypeTraits.h"
#include "Developer Settings/ML_MycelandDeveloperSettings.h"
#include "Subsystem/ML_HexGridSubsystem.h"
#include "Tiles/ML_BoardSpawner.h"

bool UML_HexPathfinder::IsTileWalkable(const AML_Tile* Tile)
{
	if (!IsValid(Tile)) return false;

	// Non-interactable tiles (the obstacle ring framing a board) are off-limits to the player even if their
	// type would otherwise allow it — they are excluded here so every consumer (board moves, BFS traversal,
	// gamepad stick, board entry/exit, nav bridge) rejects them from a single place.
	if (!Tile->IsInteractable()) return false;

	const EML_TileType Type = Tile->GetCurrentType();
	return UML_TileTypeTraits::IsWalkable(Type) && !Tile->IsBlocked();
}

bool UML_HexPathfinder::BuildPath_AxialBFS(const FIntPoint& StartAxial, const FIntPoint& GoalAxial,
                                            const TMap<FIntPoint, AML_Tile*>& GridMap,
                                            TArray<FIntPoint>& OutAxialPath)
{
	OutAxialPath.Reset();

	if (StartAxial == GoalAxial)
	{
		OutAxialPath.Add(StartAxial);
		return true;
	}

	TArray<FIntPoint> Queue;
	Queue.Reserve(GridMap.Num());
	int32 Head = 0;

	TMap<FIntPoint, FIntPoint> CameFrom;
	CameFrom.Reserve(GridMap.Num());

	Queue.Add(StartAxial);
	CameFrom.Add(StartAxial, StartAxial);

	while (Head < Queue.Num())
	{
		const FIntPoint Current = Queue[Head++];
		const AML_Tile* const* CurrentTilePtr = GridMap.Find(Current);

		for (const FIntPoint& Dir : Directions)
		{
			const FIntPoint Next = Current + Dir;

			if (CameFrom.Contains(Next))
				continue;

			const AML_Tile* const* NextTilePtr = GridMap.Find(Next);
			if (!NextTilePtr)
				continue;

			// Same step rule as the global grid (walkable + height), so a board path, the hover preview and
			// the gamepad never send the player up a step it cannot climb. A start missing from the map
			// (should not happen) falls back to the walkable-only check rather than blocking every move.
			const bool bCanStep = CurrentTilePtr ? CanStep(*CurrentTilePtr, *NextTilePtr) : IsTileWalkable(*NextTilePtr);
			if (!bCanStep)
				continue;

			CameFrom.Add(Next, Current);

			if (Next == GoalAxial)
			{
				FIntPoint Step = GoalAxial;
				while (Step != StartAxial)
				{
					OutAxialPath.Add(Step);
					Step = CameFrom[Step];
				}
				OutAxialPath.Add(StartAxial);
				Algo::Reverse(OutAxialPath);
				return true;
			}

			Queue.Add(Next);
		}
	}

	return false;
}

// ==================== Global grid movement ====================

int32 UML_HexPathfinder::HexDistance(const FIntPoint& A, const FIntPoint& B)
{
	const int32 DQ = A.X - B.X;
	const int32 DR = A.Y - B.Y;
	return (FMath::Abs(DQ) + FMath::Abs(DR) + FMath::Abs(DQ + DR)) / 2;
}

bool UML_HexPathfinder::CanStep(const AML_Tile* From, const AML_Tile* To)
{
	if (!IsValid(From) || !IsTileWalkable(To))
		return false;

	if (!UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings()->IsHeightStepAllowed(From->GetHeightLevel(), To->GetHeightLevel()))
		return false;

	if (From->GetOwner() != To->GetOwner() && !CanCrossBoardBorder(From, To))
		return false;

	return true;
}

namespace
{
	struct FML_AStarNode
	{
		FIntPoint Cell;
		int32 G = 0; // Steps from Start
		int32 H = 0; // Estimated steps to Goal
		int32 F() const { return G + H; }
	};

	// Min-heap on F, ties broken toward the node closest to Goal so the search heads straight for it.
	struct FML_AStarNodeLess
	{
		bool operator()(const FML_AStarNode& A, const FML_AStarNode& B) const
		{
			return A.F() < B.F() || (A.F() == B.F() && A.H < B.H);
		}
	};

	void ReconstructPath(const FIntPoint& Start, const FIntPoint& End, const TMap<FIntPoint, FIntPoint>& CameFrom, TArray<FIntPoint>& OutPath)
	{
		OutPath.Reset();
		for (FIntPoint Step = End; Step != Start; Step = CameFrom.FindChecked(Step))
			OutPath.Add(Step);
		OutPath.Add(Start);
		Algo::Reverse(OutPath);
	}
}

bool UML_HexPathfinder::FindPath_AStar(const FIntPoint& Start, const FIntPoint& Goal,
                                       TFunctionRef<bool(const FIntPoint& From, const FIntPoint& To)> CanStepFn,
                                       const int32 MaxExplored, const bool bAllowPartial, TArray<FIntPoint>& OutPath)
{
	OutPath.Reset();

	if (Start == Goal)
	{
		OutPath.Add(Start);
		return true;
	}

	TArray<FML_AStarNode> Open;
	TMap<FIntPoint, int32> BestG;
	TMap<FIntPoint, FIntPoint> CameFrom;

	Open.HeapPush({Start, 0, HexDistance(Start, Goal)}, FML_AStarNodeLess());
	BestG.Add(Start, 0);

	// Closest cell to Goal seen so far, for the partial path.
	FIntPoint Closest = Start;
	int32 ClosestH = HexDistance(Start, Goal);
	int32 ClosestG = 0;

	int32 Explored = 0;

	while (Open.Num() > 0)
	{
		FML_AStarNode Current;
		Open.HeapPop(Current, FML_AStarNodeLess());

		// A cheaper way to this cell was queued after this entry: this one is stale.
		if (Current.G > BestG.FindChecked(Current.Cell))
			continue;

		if (Current.Cell == Goal)
		{
			ReconstructPath(Start, Goal, CameFrom, OutPath);
			return true;
		}

		if (Current.H < ClosestH || (Current.H == ClosestH && Current.G < ClosestG))
		{
			Closest = Current.Cell;
			ClosestH = Current.H;
			ClosestG = Current.G;
		}

		if (++Explored > MaxExplored)
			break;

		for (const FIntPoint& Dir : Directions)
		{
			const FIntPoint Next = Current.Cell + Dir;
			const int32 NextG = Current.G + 1;

			if (const int32* KnownG = BestG.Find(Next); KnownG && *KnownG <= NextG)
				continue;

			if (!CanStepFn(Current.Cell, Next))
				continue;

			BestG.Add(Next, NextG);
			CameFrom.Add(Next, Current.Cell);
			Open.HeapPush({Next, NextG, HexDistance(Next, Goal)}, FML_AStarNodeLess());
		}
	}

	if (bAllowPartial)
		ReconstructPath(Start, Closest, CameFrom, OutPath);

	return false;
}

bool UML_HexPathfinder::FindPath(const UML_HexGridSubsystem& Grid, const AML_Tile* Start, const AML_Tile* Goal,
                                 const bool bAllowPartial, TArray<AML_Tile*>& OutPath)
{
	OutPath.Reset();
	if (!IsValid(Start) || !IsValid(Goal))
		return false;

	const int32 MaxExplored = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings()->PathfindingMaxExploredTiles;

	TArray<FIntPoint> CellPath;
	const bool bReachedGoal = FindPath_AStar(Start->GetGlobalAxialCoord(), Goal->GetGlobalAxialCoord(),
		[&Grid](const FIntPoint& From, const FIntPoint& To)
		{
			return CanStep(Grid.GetTileAtGlobalAxial(From), Grid.GetTileAtGlobalAxial(To));
		},
		MaxExplored, bAllowPartial, CellPath);

	OutPath.Reserve(CellPath.Num());
	for (const FIntPoint& Cell : CellPath)
	{
		AML_Tile* Tile = Grid.GetTileAtGlobalAxial(Cell);
		if (!Tile)
		{
			// Only possible for Start if its board left the grid mid-search; the path is unusable then.
			OutPath.Reset();
			return false;
		}
		OutPath.Add(Tile);
	}

	return bReachedGoal;
}

int32 UML_HexPathfinder::ComputeBoardEntryCut(TArrayView<const void* const> StepBoards, TFunctionRef<bool(const void* Board)> StopsOnEntry)
{
	for (int32 Index = 1; Index < StepBoards.Num(); ++Index)
	{
		const void* Board = StepBoards[Index];
		if (Board && Board != StepBoards[Index - 1] && StopsOnEntry(Board))
			return Index + 1;
	}
	return StepBoards.Num();
}

bool UML_HexPathfinder::TruncateAtBoardEntry(TArray<AML_Tile*>& Path)
{
	TArray<const void*, TInlineAllocator<64>> StepBoards;
	StepBoards.Reserve(Path.Num());
	for (const AML_Tile* Tile : Path)
		StepBoards.Add(Tile ? Tile->GetBoardSpawnerFromTile() : nullptr);

	const int32 Keep = ComputeBoardEntryCut(StepBoards, [](const void* Board)
	{
		return static_cast<const AML_BoardSpawner*>(Board)->ShouldStopPlayerOnEntry();
	});

	if (Keep >= Path.Num())
		return false;

	Path.SetNum(Keep);
	return true;
}
