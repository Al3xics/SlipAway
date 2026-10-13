// Copyright Myceland Team, All Rights Reserved.

#include "Subsystem/ML_WinLoseSubsystem.h"

#include "Algo/Reverse.h"
#include "Audio/ML_FMODEvents.h"
#include "Containers/Deque.h"
#include "Core/ML_CoreData.h"
#include "Core/ML_TileTypeTraits.h"
#include "Developer Settings/ML_MycelandDeveloperSettings.h"
#include "Kismet/GameplayStatics.h"
#include "Player/ML_PlayerCharacter.h"
#include "Subsystem/ML_RollBackSubsystem.h"
#include "Subsystem/ML_SoundSubsystem.h"
#include "Tiles/ML_Tile.h"
#include "Tiles/ML_TileBase.h"
#include "Tiles/TileBase/ML_TileGrass.h"

const FIntPoint UML_WinLoseSubsystem::HexDirs[6] = {
	FIntPoint(1, 0),
	FIntPoint(1, -1),
	FIntPoint(0, -1),
	FIntPoint(-1, 0),
	FIntPoint(-1, 1),
	FIntPoint(0, 1)
};

FML_GameResult UML_WinLoseSubsystem::CheckWinLose()
{
	if (!CurrentBoardSpawner)
		CurrentBoardSpawner = FindBoardSpawner();

	FML_GameResult GameResult;
	if (!IsValid(CurrentBoardSpawner) || CurrentBoardSpawner->bIsPuzzleSolved)
		return GameResult;

	if (bIsPlayerDead)
	{
		GameResult.Result = EML_WinLose::Lose;
		GameResult.bIsGameOver = true;
		bIsPlayerDead = false;
		OnLose.Broadcast();
		return GameResult;
	}

	if (AreAllGoalsConnected(CurrentBoardSpawner, EML_TileType::Tree, CachedGoalPathAllowedSet))
	{
		GameResult.Result = EML_WinLose::Win;
		CurrentBoardSpawner->bIsPuzzleSolved = true;
		KickOffWinSequence();
	}

	return GameResult;
}

void UML_WinLoseSubsystem::KickOffWinSequence()
{
	bPendingClearWinPath = true;

	// Kick off (or re-sync) the link-animation sequence.
	// OnWin fires inside BroadcastNextConnectedGoalPathTile once the queue drains.
	// Calling this here guarantees the flag is set BEFORE the queue is populated,
	// regardless of the order the Blueprint calls CheckWinLose / TriggerFindConnectedGoalCheck.
	TriggerFindConnectedGoalCheck();

	// Fallback: if the winning state produced no new path tiles to animate
	// (all tiles already resided in PreviousConnectedPathTiles), the queue is
	// empty and no repeating timer is running — nothing will drain the queue
	// and fire OnWin.  Schedule the win sequence for the next tick so the
	// call stack has fully unwound before delegates broadcast.
	const bool bQueueHasWork = QueueReadIndex < PendingConnectedGoalPathQueue.Num();
	if (!bQueueHasWork
		&& !GetWorld()->GetTimerManager().IsTimerActive(ConnectedGoalPathTimerHandle))
	{
		GetWorld()->GetTimerManager().SetTimerForNextTick([this]()
		{
			if (bPendingClearWinPath)
			{
				OnConnectedGoalPathComplete.Broadcast();
				FireWinSequence();
			}
		});
	}
}

void UML_WinLoseSubsystem::ForceWinBoard(AML_BoardSpawner* Board)
{
#if !UE_BUILD_SHIPPING
	if (!IsValid(Board))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Debug] ForceWinBoard called with an invalid board."));
		return;
	}

	if (Board->bIsPuzzleSolved)
	{
		UE_LOG(LogTemp, Log, TEXT("[Debug] ForceWinBoard: '%s' is already solved — ignoring."),
			*Board->PuzzleID.ToString());
		return;
	}

	// Make this the active board so every downstream listener keys off it — in particular
	// AML_BoardSpawner::HandlePuzzleWon bails unless CurrentBoardSpawner == itself.
	CurrentBoardSpawner = Board;
	Board->bIsPuzzleSolved = true;

	// Same pipeline as a genuine win; only the goal-connection check was skipped to get here.
	KickOffWinSequence();

	UE_LOG(LogTemp, Log, TEXT("[Debug] ForceWinBoard: forced win on '%s'."), *Board->PuzzleID.ToString());
#endif
}

bool UML_WinLoseSubsystem::CheckPlayerKilled(AML_Tile* CurrentTileOn)
{
	if (!IsValid(CurrentTileOn))
	{
		return false;
	}

	return CheckPlayerKilledByType(
		CurrentTileOn,
		CurrentTileOn->GetCurrentType()
	);
}
bool UML_WinLoseSubsystem::CheckPlayerKilledByType(
	AML_Tile* CurrentTileOn,
	EML_TileType TileType)
{
	if (bIsPlayerDead || !IsValid(CurrentTileOn))
	{
		return false;
	}

	if (UML_TileTypeTraits::IsLethalTile(TileType))
	{
		bIsPlayerDead = true;

		// Immediate event.
		// Bind your death animation to this.
		OnDeath.Broadcast();

		return true;
	}

	return false;
}
bool UML_WinLoseSubsystem::AreAllGoalsConnected(
	AML_BoardSpawner* Board,
	EML_TileType GoalType,
	const TSet<EML_TileType>& AllowedSet)
{
	if (!IsValid(Board))
		return false;

	const TMap<FIntPoint, AML_Tile*>& Grid = Board->GetGridMapRef();
	if (Grid.Num() == 0)
		return false;

	const TArray<FIntPoint> GoalAxials = CollectGoalAxials(Grid, GoalType);
	if (GoalAxials.Num() <= 1)
		return true;

	auto CanTraverse = [&](AML_Tile* Tile) -> bool
	{
		if (!IsValid(Tile)) return false;
		const EML_TileType TileType = Tile->GetCurrentType();
		return TileType == GoalType || AllowedSet.Contains(TileType);
	};

	TSet<FIntPoint> Visited;
	TMap<FIntPoint, FIntPoint> Parent;
	RunBFS(Grid, GoalAxials[0], CanTraverse, Visited, Parent);

	for (const FIntPoint& GoalAxial : GoalAxials)
	{
		if (!Visited.Contains(GoalAxial))
			return false;
	}

	return true;
}

bool UML_WinLoseSubsystem::FindConnectedGoalGroups(
	AML_BoardSpawner* Board,
	EML_TileType GoalType,
	const TArray<EML_TileType>& AllowedPathTypes,
	bool bDisallowBlocked,
	int32 MinGoalsInGroup)
{
	return FindConnectedGoalGroups(Board, GoalType, BuildAllowedSet(AllowedPathTypes), bDisallowBlocked, MinGoalsInGroup);
}

bool UML_WinLoseSubsystem::FindConnectedGoalGroups(
	AML_BoardSpawner* Board,
	EML_TileType GoalType,
	const TSet<EML_TileType>& AllowedSet,
	bool bDisallowBlocked,
	int32 MinGoalsInGroup)
{
	ConnectedGoalGroups.Reset();

	if (!IsValid(Board))
	{
		return false;
	}

	const TMap<FIntPoint, AML_Tile*>& Grid = Board->GetGridMapRef();
	if (Grid.Num() == 0)
	{
		return false;
	}

	const TArray<FIntPoint> GoalAxials = CollectGoalAxials(Grid, GoalType, bDisallowBlocked);

	const int32 RequiredGoalsPerPath = FMath::Max(2, MinGoalsInGroup);
	if (GoalAxials.Num() < RequiredGoalsPerPath)
	{
		return false;
	}

	const int32 N = GoalAxials.Num();

	// LocalUsedTiles defined before CanTraverse — both lambdas capture it by reference.
	// Seeded with only currently-valid path tiles (AllowedSet) from prior calls so that
	// goal tiles removed by undo don't remain traversable as stale waypoints.
	// Goal tiles enter LocalUsedTiles only after Phase 1 confirms they are connected.
	TSet<AML_Tile*> LocalUsedTiles;
	for (AML_Tile* Tile : PreviousConnectedPathTiles)
	{
		if (IsValid(Tile) && AllowedSet.Contains(Tile->GetCurrentType()))
			LocalUsedTiles.Add(Tile);
	}

	// Once a goal tile enters LocalUsedTiles it is treated exactly like grass or water:
	// traversable and cost-0. This lets subsequent paths route THROUGH linked goals
	// instead of creating parallel detours around them.
	auto CanTraverse = [&](AML_Tile* Tile) -> bool
	{
		if (!IsValid(Tile)) return false;
		if (bDisallowBlocked && Tile->IsBlocked()) return false;
		if (AllowedSet.Contains(Tile->GetCurrentType())) return true;
		return Tile->GetCurrentType() == GoalType && LocalUsedTiles.Contains(Tile);
	};

	auto GetCost = [&](AML_Tile* Tile) -> int32
	{
		return LocalUsedTiles.Contains(Tile) ? 0 : 1;
	};

	// FindBridge: cheapest entry tile adjacent to TargetGoal that the BFS reached.
	// Accepts AllowedSet tiles and linked goal tiles (both are valid waypoints).
	auto FindBridge = [&](
		const FIntPoint& TargetGoal,
		const TSet<FIntPoint>& Visited,
		const TMap<FIntPoint, int32>& DistMap) -> FIntPoint
	{
		FIntPoint Bridge = FIntPoint(INT_MAX, INT_MAX);
		int32 BestDist = INT_MAX;
		for (const FIntPoint& Dir : HexDirs)
		{
			const FIntPoint Neighbor = TargetGoal + Dir;
			if (!Visited.Contains(Neighbor)) continue;
			AML_Tile* const* NPtr = Grid.Find(Neighbor);
			if (!NPtr || !IsValid(*NPtr)) continue;
			AML_Tile* NTile = *NPtr;
			if (!AllowedSet.Contains(NTile->GetCurrentType()) && !(NTile->GetCurrentType() == GoalType && LocalUsedTiles.Contains(NTile))) continue;
			const int32* D = DistMap.Find(Neighbor);
			if (!D) continue;
			if (*D < BestDist) { BestDist = *D; Bridge = Neighbor; }
		}
		return Bridge;
	};

	// Phase 0: all-pairs 0-1 BFS with the current LocalUsedTiles as cost-0.
	// CanTraverse allows linked goal tiles, so distances reflect the real traversable
	// network (goals act as waypoints). Edge weight = new tiles needed for that pair.
	struct FPairEdge { int32 i, j, Dist; };
	TArray<FPairEdge> AllEdges;

	for (int32 i = 0; i < N; ++i)
	{
		TSet<FIntPoint> Vis;
		TMap<FIntPoint, FIntPoint> Par;
		TMap<FIntPoint, int32> Dist;
		RunZeroOneBFS(Grid, GoalAxials[i], CanTraverse, GetCost, Vis, Par, Dist);

		for (int32 j = i + 1; j < N; ++j)
		{
			int32 BridgeDist = INT_MAX;
			for (const FIntPoint& Dir : HexDirs)
			{
				const FIntPoint Neighbor = GoalAxials[j] + Dir;
				if (!Vis.Contains(Neighbor)) continue;
				AML_Tile* const* NPtr = Grid.Find(Neighbor);
				if (!NPtr || !IsValid(*NPtr)) continue;
				AML_Tile* NTile = *NPtr;
				if (!AllowedSet.Contains(NTile->GetCurrentType()) && !(NTile->GetCurrentType() == GoalType && LocalUsedTiles.Contains(NTile))) continue;
				const int32* D = Dist.Find(Neighbor);
				if (D && *D < BridgeDist) BridgeDist = *D;
			}
			if (BridgeDist != INT_MAX) AllEdges.Add({i, j, BridgeDist + 1});
		}
	}

	if (AllEdges.IsEmpty()) return false;

	// Sort descending: pairs needing the most new tiles go first — they form the backbone.
	// Cheaper pairs (already routable through established tiles) are processed after and
	// will naturally pass through the corridor that earlier edges built up.
	AllEdges.Sort([](const FPairEdge& A, const FPairEdge& B) { return A.Dist > B.Dist; });

	// Kruskal's maximum spanning tree: N-1 essential links, no redundant connections.
	TArray<int32> UF; UF.SetNum(N);
	for (int32 k = 0; k < N; ++k) UF[k] = k;
	auto UFFind = [&](int32 x) -> int32
	{
		while (UF[x] != x) { UF[x] = UF[UF[x]]; x = UF[x]; }
		return x;
	};

	TArray<FPairEdge> MSTEdges;
	for (const FPairEdge& E : AllEdges)
	{
		const int32 ra = UFFind(E.i), rb = UFFind(E.j);
		if (ra == rb) continue;
		UF[ra] = rb;
		MSTEdges.Add(E);
		if (MSTEdges.Num() == N - 1) break;
	}

	if (MSTEdges.IsEmpty()) return false;

	// Pre-add every goal tile that will be linked this call before computing any path.
	// Phase 0 only knew about PreviousConnectedPathTiles, so goals discovered via the MST
	// weren't waypoints yet during distance calculation. Adding them all now means every
	// BFS in Phase 1 sees the complete waypoint network — the same context every time,
	// regardless of which MST edge happens to run first.
	for (const FPairEdge& E : MSTEdges)
	{
		if (AML_Tile* T = Grid.FindRef(GoalAxials[E.i]); IsValid(T)) LocalUsedTiles.Add(T);
		if (AML_Tile* T = Grid.FindRef(GoalAxials[E.j]); IsValid(T)) LocalUsedTiles.Add(T);
	}

	// Phase 1: 0-1 BFS per MST edge. All linked goal tiles are already cost-0 waypoints.
	// Path tiles still grow after each edge so later links can reuse earlier corridors.
	for (const FPairEdge& E : MSTEdges)
	{
		TSet<FIntPoint> Visited;
		TMap<FIntPoint, FIntPoint> Parent;
		TMap<FIntPoint, int32> Dist;
		RunZeroOneBFS(Grid, GoalAxials[E.i], CanTraverse, GetCost, Visited, Parent, Dist);

		const FIntPoint Bridge = FindBridge(GoalAxials[E.j], Visited, Dist);
		if (Bridge == FIntPoint(INT_MAX, INT_MAX)) continue;

		TArray<FIntPoint> PathAxials;
		if (!BuildPathAxialsFromParent(GoalAxials[E.i], Bridge, Parent, PathAxials)) continue;
		PathAxials.Add(GoalAxials[E.j]);

		for (const FIntPoint& Axial : PathAxials)
		{
			if (AML_Tile* const* TilePtr = Grid.Find(Axial))
				if (IsValid(*TilePtr)) LocalUsedTiles.Add(*TilePtr);
		}

		TArray<AML_Tile*> PathTilesLocal;
		if (!ConvertAxialsToTiles(Grid, PathAxials, PathTilesLocal)) continue;

		FML_TileGroup Group;
		Group.Tiles = MoveTemp(PathTilesLocal);
		if (AML_Tile* GoalA = Grid.FindRef(GoalAxials[E.i]); IsValid(GoalA)) Group.Goals.Add(GoalA);
		if (AML_Tile* GoalB = Grid.FindRef(GoalAxials[E.j]); IsValid(GoalB)) Group.Goals.Add(GoalB);

		if (Group.Goals.Num() >= RequiredGoalsPerPath)
			ConnectedGoalGroups.Add(MoveTemp(Group));
	}

	return ConnectedGoalGroups.Num() > 0;
}

void UML_WinLoseSubsystem::TriggerFindConnectedGoalCheck()
{
	// Once the puzzle is solved the win path is final. Only the winning turn itself
	// (bPendingClearWinPath still set) may recompute; anything later — e.g. hub tile
	// changes running a wave while the player still stands on the solved board — must
	// not, because FireWinSequence converts corridor Water tiles to WaterPath (not a
	// win-path type), so a recompute would reroute the path and replay the link glow.
	if (IsValid(CurrentBoardSpawner) && CurrentBoardSpawner->bIsPuzzleSolved && !bPendingClearWinPath)
		return;

	FindConnectedGoalGroups(
		CurrentBoardSpawner,
		EML_TileType::Tree,
		CachedGoalPathAllowedSet,
		false,
		2);

	TSet<AML_Tile*> CurrentConnectedPathTiles;
	for (const FML_TileGroup& Group : ConnectedGoalGroups)
	{
		for (AML_Tile* Tile : Group.Tiles)
		{
			if (IsValid(Tile))
			{
				// Later MST links reuse earlier corridors, so the same tile can appear
				// in several groups: dedup against this call's set, not just the queue.
				bool bAlreadyInSet = false;
				CurrentConnectedPathTiles.Add(Tile, &bAlreadyInSet);
				if (!bAlreadyInSet && !PreviousConnectedPathTiles.Contains(Tile))
					PendingConnectedGoalPathQueue.Add(Tile);
			}
		}
	}

	TArray<AML_Tile*> DisconnectedTiles;
	for (AML_Tile* Tile : PreviousConnectedPathTiles)
	{
		if (!CurrentConnectedPathTiles.Contains(Tile))
		{
			DisconnectedTiles.Add(Tile);

			for (int32 i = QueueReadIndex; i < PendingConnectedGoalPathQueue.Num(); ++i)
			{
				if (PendingConnectedGoalPathQueue[i] == Tile)
				{
					PendingConnectedGoalPathQueue[i] = nullptr;
					break;
				}
			}
		}
	}

	PreviousConnectedPathTiles = MoveTemp(CurrentConnectedPathTiles);

	if (DisconnectedTiles.Num() > 0)
		OnDisconnectedGoalPathTile.Broadcast(DisconnectedTiles);

	if (QueueReadIndex < PendingConnectedGoalPathQueue.Num()
		&& !GetWorld()->GetTimerManager().IsTimerActive(ConnectedGoalPathTimerHandle))
	{
		GetWorld()->GetTimerManager().SetTimer(ConnectedGoalPathTimerHandle, this,
			&UML_WinLoseSubsystem::BroadcastNextConnectedGoalPathTile, DevSettings->WinTileDelay, true);
	}
}

void UML_WinLoseSubsystem::TriggerConnectedGoalAnimationForBoard(AML_BoardSpawner* Board, bool bImmediate)
{
	if (!IsValid(Board)) return;

	// This can run before OnWorldBeginPlay (e.g. a hub revealing its solved connections
	// during load), so make sure the allowed-path set is populated first.
	if (CachedGoalPathAllowedSet.Num() == 0)
		CachedGoalPathAllowedSet = UML_TileTypeTraits::GetWinPathTypes();

	// Compute connected groups on the requested board — does NOT alter
	// CurrentBoardSpawner or PreviousConnectedPathTiles, so the active
	// board's pathfinding state is completely unaffected.
	FindConnectedGoalGroups(Board, EML_TileType::Tree, CachedGoalPathAllowedSet, false, 2);

	bool bAddedAny = false;
	for (const FML_TileGroup& Group : ConnectedGoalGroups)
	{
		for (AML_Tile* Tile : Group.Tiles)
		{
			if (!IsValid(Tile)) continue;
			// PersistentAnimatedTiles tracks what has already been shown so
			// re-entering the hub (or solving a second puzzle on the same hub)
			// never re-plays the animation for tiles that are already lit.
			if (PersistentAnimatedTiles.Contains(Tile)) continue;

			PersistentAnimatedTiles.Add(Tile);

			if (bImmediate)
			{
				// Reveal instantly (load): broadcast now so the shared queue/timer aren't
				// involved and a board change can't cancel a partially-played reveal.
				OnConnectedGoalPathTile.Broadcast(Tile);
			}
			else
			{
				PendingConnectedGoalPathQueue.Add(Tile);
				bAddedAny = true;
			}
		}
	}

	if (bAddedAny
		&& !GetWorld()->GetTimerManager().IsTimerActive(ConnectedGoalPathTimerHandle))
	{
		GetWorld()->GetTimerManager().SetTimer(ConnectedGoalPathTimerHandle, this,
			&UML_WinLoseSubsystem::BroadcastNextConnectedGoalPathTile, DevSettings->WinTileDelay, true);
	}
}

void UML_WinLoseSubsystem::ResetConnectedGoalAnimationForBoard(AML_BoardSpawner* Board)
{
	if (!IsValid(Board)) return;

	// Only this board's tiles are cleared — PersistentAnimatedTiles is a flat set shared
	// across boards (incl. the hub), so scope by iterating the board's own grid.
	TArray<AML_Tile*> Disconnected;
	for (AML_Tile* Tile : Board->GetGridTiles())
	{
		if (IsValid(Tile) && PersistentAnimatedTiles.Remove(Tile) > 0)
			Disconnected.Add(Tile);
	}

	if (Disconnected.Num() > 0)
		OnDisconnectedGoalPathTile.Broadcast(Disconnected);
}

void UML_WinLoseSubsystem::BroadcastNextConnectedGoalPathTile()
{
	while (QueueReadIndex < PendingConnectedGoalPathQueue.Num()
		&& PendingConnectedGoalPathQueue[QueueReadIndex] == nullptr)
	{
		++QueueReadIndex;
	}

	if (QueueReadIndex >= PendingConnectedGoalPathQueue.Num())
	{
		PendingConnectedGoalPathQueue.Reset();
		QueueReadIndex = 0;
		GetWorld()->GetTimerManager().ClearTimer(ConnectedGoalPathTimerHandle);

		if (bPendingClearWinPath)
		{
			if (UML_SoundSubsystem* SoundSubsystem = UML_SoundSubsystem::Get(this))
			{
				SoundSubsystem->StartSound2DByPath(MLFMODEvents::TreeLinkMotif);
			}

			OnConnectedGoalPathComplete.Broadcast();
			GetWorld()->GetTimerManager().SetTimer(
				ConnectedWinTimerHandle,
				this,
				&UML_WinLoseSubsystem::FireWinSequence,
				DevSettings->WinDelay,
				false
			);			
		}

		return;
	}

	AML_Tile* Next = PendingConnectedGoalPathQueue[QueueReadIndex++];
	if (IsValid(Next))
		OnConnectedGoalPathTile.Broadcast(Next);
}
void UML_WinLoseSubsystem::ForceBoardWin(AML_BoardSpawner* Board)
{
	if (!IsValid(Board))
		return;

	if (Board->bIsPuzzleSolved)
		return;

	CurrentBoardSpawner = Board;
	Board->bIsPuzzleSolved = true;

	FireWinSequence();
}

void UML_WinLoseSubsystem::ReplayOnWinForBoard(AML_BoardSpawner* Board)
{
	if (!IsValid(Board))
		return;

	// Listeners read CurrentBoardSpawner to know which board won — point it at Board, then fire OnWin
	// only. No FireWinSequence side-effects (victory sound / ClearWinPath / deferred OnWinPathSettled).
	CurrentBoardSpawner = Board;

	// Lets listeners tell this replay apart from a real solve, so the ones that are a reaction to
	// winning (the win cinematic) can sit it out while the ones that rebuild state still run.
	bIsReplayingWinForLoad = true;
	OnWin.Broadcast();
	bIsReplayingWinForLoad = false;
}

void UML_WinLoseSubsystem::FireWinSequence()
{
	OnWin.Broadcast();
	if (UML_SoundSubsystem* SoundSubsystem = UML_SoundSubsystem::Get(this))
	{
		if (const AML_Tile* PlayerTile = GetPlayerCurrentTile())
		{
			const FTransform PlayerTransform(FRotator::ZeroRotator, PlayerTile->GetActorLocation());
			SoundSubsystem->StartSoundAtLocationByPath(MLFMODEvents::AvatarVictoryVocal, PlayerTransform);
		}
	}

	bPendingClearWinPath = false;

	if (!IsValid(CurrentBoardSpawner))
		return;

	// First pass: Entry → Exit (converts Water → WaterPath along the way).
	for (const auto& WaterPath : CurrentBoardSpawner->WaterPaths)
	{
		ClearWinPath(
			CurrentBoardSpawner,
			WaterPath.EntryTile,
			WaterPath.ExitTile,
			{EML_TileType::Grass, EML_TileType::Water, EML_TileType::WaterPath, EML_TileType::Dirt}
		);
	}

	// Defer the player pass by one tick so UpdateClassAtRuntime changes
	// from the first pass are fully applied before the second BFS runs. With
	// WaterPath allowed, this BFS rides the Entry→Exit trunk for free instead
	// of carving a duplicate lily trail through open water.
	GetWorld()->GetTimerManager().SetTimerForNextTick([this]()
	{
		if (!IsValid(CurrentBoardSpawner))
			return;

		AML_Tile* PlayerTile = GetPlayerCurrentTile();
		if (!IsValid(PlayerTile))
			return;

		for (const auto& WaterPath : CurrentBoardSpawner->WaterPaths)
		{
			// Entry/Exit can be authored in either order — target whichever
			// endpoint is closest to the player; the trunk connects both.
			const AML_Tile* Target = WaterPath.ExitTile;
			if (IsValid(WaterPath.EntryTile)
				&& (!IsValid(WaterPath.ExitTile)
					|| FVector::DistSquared(PlayerTile->GetActorLocation(), WaterPath.EntryTile->GetActorLocation())
						< FVector::DistSquared(PlayerTile->GetActorLocation(), WaterPath.ExitTile->GetActorLocation())))
			{
				Target = WaterPath.EntryTile;
			}

			ClearWinPath(
				CurrentBoardSpawner,
				PlayerTile,
				Target,
				{EML_TileType::Grass, EML_TileType::Water, EML_TileType::WaterPath, EML_TileType::Dirt}
			);
		}

		OnWinPathSettled.Broadcast();
	});
}

AML_Tile* UML_WinLoseSubsystem::GetPlayerCurrentTile() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	AML_PlayerCharacter* PlayerCharacter = Cast<AML_PlayerCharacter>(
		UGameplayStatics::GetPlayerCharacter(World, 0));

	if (!PlayerCharacter)
	{
		return nullptr;
	}

	AML_Tile* PlayerCurrentTile = PlayerCharacter->CurrentTileOn;
	return IsValid(PlayerCurrentTile) ? PlayerCurrentTile : nullptr;
}

void UML_WinLoseSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	AML_PlayerCharacter* Player = Cast<AML_PlayerCharacter>(
		UGameplayStatics::GetPlayerCharacter(&InWorld, 0));

	if (Player)
	{
		BoundPlayer = Player;
		// Puzzles only: on a world board there is no current puzzle (CurrentBoardSpawner stays null).
		Player->OnPuzzleBoardChanged.AddDynamic(this, &UML_WinLoseSubsystem::HandleBoardChanged);
	}

	if (UML_RollBackSubsystem* RollBack = InWorld.GetSubsystem<UML_RollBackSubsystem>())
	{
		RollBack->OnUndoAnimating.AddDynamic(this, &UML_WinLoseSubsystem::HandleUndoAnimating);
		RollBack->OnResetAnimating.AddDynamic(this, &UML_WinLoseSubsystem::HandleResetAnimating);
	}

	CachedGoalPathAllowedSet = UML_TileTypeTraits::GetWinPathTypes();
	DevSettings = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();
}

void UML_WinLoseSubsystem::ResetConnectedGoalPathState()
{
	PreviousConnectedPathTiles.Reset();
	PendingConnectedGoalPathQueue.Reset();
	QueueReadIndex = 0;
	GetWorld()->GetTimerManager().ClearTimer(ConnectedGoalPathTimerHandle);
}

void UML_WinLoseSubsystem::HandleBoardChanged(const AML_Tile* OldTile, const AML_Tile* NewTile)
{
	if (PreviousConnectedPathTiles.Num() > 0
		&& IsValid(CurrentBoardSpawner) && !CurrentBoardSpawner->bIsPuzzleSolved)
	{
		TArray<AML_Tile*> AllConnected = PreviousConnectedPathTiles.Array();
		ResetConnectedGoalPathState();
		OnDisconnectedGoalPathTile.Broadcast(AllConnected);
	}
	else
	{
		ResetConnectedGoalPathState();
	}

	if (!IsValid(NewTile))
	{
		CurrentBoardSpawner = nullptr;
		return;
	}

	CurrentBoardSpawner = FindBoardSpawner();
}

AML_BoardSpawner* UML_WinLoseSubsystem::FindBoardSpawner() const
{
	AML_Tile* ChildTile = GetPlayerCurrentTile();
	if (!IsValid(ChildTile))
	{
		return nullptr;
	}

	AActor* BoardActor = ChildTile->GetAttachParentActor();
	if (!BoardActor)
	{
		return nullptr;
	}

	AML_BoardSpawner* RetrievedBoardSpawner = Cast<AML_BoardSpawner>(BoardActor);
	if (!RetrievedBoardSpawner)
	{
		return nullptr;
	}

	// A world board is never the current puzzle.
	if (RetrievedBoardSpawner->IsWorldBoard())
	{
		return nullptr;
	}

	UE_LOG(LogTemp, Log, TEXT("Board found: %s at %s"),
		*RetrievedBoardSpawner->GetName(),
		*RetrievedBoardSpawner->GetActorLocation().ToString());

	return RetrievedBoardSpawner;
}

void UML_WinLoseSubsystem::ClearWinPath(
	const AML_BoardSpawner* Board,
	const AML_Tile* StartTile,
	const AML_Tile* GoalTile,
	const TArray<EML_TileType>& AllowedPathTypes) const
{
	if (!IsValid(Board) || !IsValid(StartTile) || !IsValid(GoalTile))
	{
		return;
	}

	const TMap<FIntPoint, AML_Tile*>& Grid = Board->GetGridMapRef();
	if (Grid.Num() == 0)
	{
		return;
	}

	const TSet<EML_TileType> AllowedSet = BuildAllowedSet(AllowedPathTypes);

	const FIntPoint Start = StartTile->GetAxialCoord();
	const FIntPoint Goal = GoalTile->GetAxialCoord();

	auto CanTraverse = [&](AML_Tile* Tile) -> bool
	{
		if (!IsValid(Tile))
		{
			return false;
		}

		return Tile == StartTile
			|| Tile == GoalTile
			|| AllowedSet.Contains(Tile->GetCurrentType());
	};

	// Water costs 1, everything else 0: among all valid routes, this converts
	// the fewest Water tiles into lilies (a plain BFS minimizes tile count and
	// can cross open water even when an equal-length route stays dry).
	auto GetCost = [](AML_Tile* Tile) -> int32
	{
		return UML_TileTypeTraits::IsWaterType(Tile->GetCurrentType()) ? 1 : 0;
	};

	TSet<FIntPoint> Visited;
	TMap<FIntPoint, FIntPoint> Parent;
	TMap<FIntPoint, int32> Dist;
	RunZeroOneBFS(Grid, Start, CanTraverse, GetCost, Visited, Parent, Dist);

	if (Start != Goal && !Visited.Contains(Goal))
	{
		return;
	}

	TArray<FIntPoint> PathAxials;
	if (Start == Goal)
	{
		PathAxials.Add(Start);
	}
	else if (!BuildPathAxialsFromParent(Start, Goal, Parent, PathAxials))
	{
		return;
	}

	for (const FIntPoint& Axial : PathAxials)
	{
		AML_Tile* const* TilePtr = Grid.Find(Axial);
		if (!TilePtr || !IsValid(*TilePtr))
		{
			continue;
		}

		if (UML_TileTypeTraits::IsWaterType((*TilePtr)->GetCurrentType()))
		{
			(*TilePtr)->UpdateClassAtRuntime(EML_TileType::WaterPath, CurrentBoardSpawner->WaterChangeTile);

			if (UML_SoundSubsystem* SoundSubsystem = UML_SoundSubsystem::Get(this))
			{
				const FTransform TileTransform(FRotator::ZeroRotator, (*TilePtr)->GetActorLocation());
				SoundSubsystem->StartSoundAtLocationByPath(MLFMODEvents::TileBridgeGrow, TileTransform);
			}
		}
	}
}

TSet<EML_TileType> UML_WinLoseSubsystem::BuildAllowedSet(
	const TArray<EML_TileType>& AllowedPathTypes) const
{
	TSet<EML_TileType> AllowedSet;
	AllowedSet.Reserve(AllowedPathTypes.Num());

	for (const EML_TileType Type : AllowedPathTypes)
	{
		AllowedSet.Add(Type);
	}

	return AllowedSet;
}

TArray<FIntPoint> UML_WinLoseSubsystem::CollectGoalAxials(
	const TMap<FIntPoint, AML_Tile*>& Grid,
	EML_TileType GoalType,
	bool bDisallowBlocked) const
{
	TArray<FIntPoint> GoalAxials;

	for (const auto& Pair : Grid)
	{
		AML_Tile* Tile = Pair.Value;
		if (!IsValid(Tile))
		{
			continue;
		}

		if (Tile->GetCurrentType() != GoalType)
		{
			continue;
		}

		if (bDisallowBlocked && Tile->IsBlocked())
		{
			continue;
		}

		GoalAxials.Add(Pair.Key);
	}

	return GoalAxials;
}

void UML_WinLoseSubsystem::RunBFS(
	const TMap<FIntPoint, AML_Tile*>& Grid,
	const FIntPoint& Start,
	TFunctionRef<bool(AML_Tile*)> CanTraverse,
	TSet<FIntPoint>& OutVisited,
	TMap<FIntPoint, FIntPoint>& OutParent) const
{
	OutVisited.Reset();
	OutParent.Reset();

	TQueue<FIntPoint> Queue;

	OutVisited.Reserve(Grid.Num());
	OutParent.Reserve(Grid.Num());

	OutVisited.Add(Start);
	Queue.Enqueue(Start);

	while (!Queue.IsEmpty())
	{
		FIntPoint Current;
		Queue.Dequeue(Current);

		for (const FIntPoint& Dir : HexDirs)
		{
			const FIntPoint Next = Current + Dir;
			if (OutVisited.Contains(Next))
			{
				continue;
			}

			AML_Tile* const* NextPtr = Grid.Find(Next);
			if (!NextPtr)
			{
				continue;
			}

			AML_Tile* NextTile = *NextPtr;
			if (!CanTraverse(NextTile))
			{
				continue;
			}

			OutVisited.Add(Next);
			OutParent.Add(Next, Current);
			Queue.Enqueue(Next);
		}
	}
}

void UML_WinLoseSubsystem::RunZeroOneBFS(
	const TMap<FIntPoint, AML_Tile*>& Grid,
	const FIntPoint& Start,
	TFunctionRef<bool(AML_Tile*)> CanTraverse,
	TFunctionRef<int32(AML_Tile*)> GetCost,
	TSet<FIntPoint>& OutVisited,
	TMap<FIntPoint, FIntPoint>& OutParent,
	TMap<FIntPoint, int32>& OutDist) const
{
	OutVisited.Reset();
	OutParent.Reset();
	OutDist.Reset();

	OutDist.Reserve(Grid.Num());
	OutDist.Add(Start, 0);

	TDeque<FIntPoint> Deque;
	Deque.PushLast(Start);

	while (!Deque.IsEmpty())
	{
		const FIntPoint Current = Deque.First();
		Deque.PopFirst();

		if (OutVisited.Contains(Current))
			continue;
		OutVisited.Add(Current);

		const int32 CurrentDist = OutDist[Current];

		for (const FIntPoint& Dir : HexDirs)
		{
			const FIntPoint Next = Current + Dir;
			if (OutVisited.Contains(Next))
				continue;

			AML_Tile* const* NextPtr = Grid.Find(Next);
			if (!NextPtr)
				continue;

			AML_Tile* NextTile = *NextPtr;
			if (!CanTraverse(NextTile))
				continue;

			const int32 Cost = GetCost(NextTile);
			const int32 NewDist = CurrentDist + Cost;

			int32* ExistingDist = OutDist.Find(Next);
			if (ExistingDist && NewDist >= *ExistingDist)
				continue;

			OutDist.Add(Next, NewDist);
			OutParent.Add(Next, Current);

			if (Cost == 0)
				Deque.PushFirst(Next);
			else
				Deque.PushLast(Next);
		}
	}
}

bool UML_WinLoseSubsystem::BuildPathAxialsFromParent(
	const FIntPoint& Start,
	const FIntPoint& Target,
	const TMap<FIntPoint, FIntPoint>& Parent,
	TArray<FIntPoint>& OutAxials) const
{
	OutAxials.Reset();

	FIntPoint Node = Target;
	OutAxials.Add(Node);

	while (Node != Start)
	{
		const FIntPoint* Prev = Parent.Find(Node);
		if (!Prev)
		{
			return false;
		}

		Node = *Prev;
		OutAxials.Add(Node);
	}

	Algo::Reverse(OutAxials);
	return true;
}

bool UML_WinLoseSubsystem::ConvertAxialsToTiles(
	const TMap<FIntPoint, AML_Tile*>& Grid,
	const TArray<FIntPoint>& Axials,
	TArray<AML_Tile*>& OutTiles) const
{
	OutTiles.Reset();
	OutTiles.Reserve(Axials.Num());

	for (const FIntPoint& Axial : Axials)
	{
		if (AML_Tile* const* TilePtr = Grid.Find(Axial))
		{
			OutTiles.Add(*TilePtr);
		}
	}

	return OutTiles.Num() > 0;
}

void UML_WinLoseSubsystem::HandleUndoAnimating(bool bIsAnimating)
{
	if (!bIsAnimating)
		TriggerFindConnectedGoalCheck();
}

void UML_WinLoseSubsystem::HandleResetAnimating(bool bIsAnimating)
{
	if (bIsAnimating)
	{
		TArray<AML_Tile*> AllConnected = PreviousConnectedPathTiles.Array();
		PreviousConnectedPathTiles.Reset();
		PendingConnectedGoalPathQueue.Reset();
		QueueReadIndex = 0;
		GetWorld()->GetTimerManager().ClearTimer(ConnectedGoalPathTimerHandle);

		if (AllConnected.Num() > 0)
			OnDisconnectedGoalPathTile.Broadcast(AllConnected);
	}
	else
	{
		TriggerFindConnectedGoalCheck();
	}
}
