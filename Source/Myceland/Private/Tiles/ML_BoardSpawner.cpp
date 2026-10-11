// Copyright Myceland Team, All Rights Reserved.


#include "Tiles/ML_BoardSpawner.h"
#include "Core/ML_TileTypeTraits.h"
#include "Tiles/ML_Tile.h"
#include "Data Asset/ML_BiomeTileSet.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Actors/ML_CameraRail.h"
#include "Actors/ML_WaterNavPath.h"
#include "Developer Settings/ML_MycelandDeveloperSettings.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Player/ML_HexPathfinder.h"
#include "Player/ML_PlayerController.h"
#include "PuzzleGeneration/ML_PuzzleSolver.h"
#include "Save System/ML_SaveSubsystem.h"
#include "Subsystem/ML_CinematicSubsystem.h"
#include "Subsystem/ML_HexGridSubsystem.h"
#include "Subsystem/ML_WinLoseSubsystem.h"


AML_BoardSpawner::AML_BoardSpawner()
{
	PrimaryActorTick.bCanEverTick = false;
}

void AML_BoardSpawner::Destroyed()
{
	ClearTiles();
	Super::Destroyed();
}

void AML_BoardSpawner::SetGlowEnabled(bool bEnabled)
{
	if (bGlowEnabled == bEnabled) return;
	bGlowEnabled = bEnabled;

	// Turning OFF: the hover component's per-tick same-tile short-circuit would leave a stale
	// glow on the currently hovered tile, so clear the active glow immediately.
	if (!bGlowEnabled)
		if (AML_PlayerController* PC = Cast<AML_PlayerController>(GetWorld()->GetFirstPlayerController()))
			PC->ClearActiveGlow();
}

void AML_BoardSpawner::SetBoardTransitionEnabled(bool bEnabled)
{
	if (bBoardTransitionEnabled == bEnabled) return;
	bBoardTransitionEnabled = bEnabled;

	// Turning OFF while the player is inside this board: eject them back to free movement.
	if (!bBoardTransitionEnabled)
		if (AML_PlayerController* PC = Cast<AML_PlayerController>(GetWorld()->GetFirstPlayerController()))
			PC->NotifyBoardTransitionDisabled(this);
}

// ==================== Board Locks (Required Puzzles) ====================

// Solved state of a board, read from the save when it has an ID — authoritative and independent of
// the order boards run BeginPlay in — and falling back to its runtime flag otherwise.
static bool IsBoardSolvedNow(const AML_BoardSpawner* Board, const UML_SaveSubsystem* SaveSys)
{
	if (!IsValid(Board))
		return false;

	if (SaveSys && Board->PuzzleID.IsValid())
		return SaveSys->IsPuzzleSolved(Board->PuzzleID.GetTagName());

	return Board->bIsPuzzleSolved;
}

EML_BoardLockAction AML_BoardSpawner::ResolveLockAction(bool bPrerequisitesSolved, bool bLockedByRule, bool bIsSolved)
{
	// Still waiting on a prerequisite: lock it, unless the rule already did.
	if (!bPrerequisitesSolved)
		return bLockedByRule ? EML_BoardLockAction::None : EML_BoardLockAction::Lock;

	// Open, and the rule is holding nothing: leave the board's switches alone.
	if (!bLockedByRule)
		return EML_BoardLockAction::None;

	// The prerequisites are met, so hand the switches back — unless the board was solved while it was
	// locked (a cheat win, or a save restored underneath): a solved board stays off like any other.
	return bIsSolved ? EML_BoardLockAction::Release : EML_BoardLockAction::Unlock;
}

bool AML_BoardSpawner::ArePrerequisitesSolved() const
{
	const UML_SaveSubsystem* SaveSys = GetSaveSubsystem();

	for (const TObjectPtr<AML_BoardSpawner>& Required : RequiredPuzzles)
	{
		// An empty slot (or a board pointing at itself) is an authoring mistake, not a lock — skipping it
		// keeps the board playable instead of stranding it behind a prerequisite that can never be solved.
		if (!IsValid(Required) || Required == this)
			continue;

		if (!IsBoardSolvedNow(Required, SaveSys))
			return false;
	}

	return true;
}

void AML_BoardSpawner::RefreshLockState()
{
	// Opt-in: a board with no Required Puzzles keeps whatever its switches are set to. That is what
	// makes this safe for the hub and for every board authored before the rule existed — the rule can
	// only ever take back what it switched off itself.
	if (RequiredPuzzles.IsEmpty())
		return;

	// A disabled rule reads as "every prerequisite is solved", so turning the setting off in Project
	// Settings gives their switches back to the boards it had locked instead of stranding them.
	const bool bRuleEnabled = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings()->bLockUnreachedPuzzleBoards;
	const bool bPrerequisitesSolved = !bRuleEnabled || ArePrerequisitesSolved();

	switch (ResolveLockAction(bPrerequisitesSolved, bLockedByPrerequisites, IsBoardSolvedNow(this, GetSaveSubsystem())))
	{
	case EML_BoardLockAction::Lock:
		bLockedByPrerequisites = true;
		SetGlowEnabled(false);
		SetBoardTransitionEnabled(false);
		break;

	case EML_BoardLockAction::Unlock:
		bLockedByPrerequisites = false;
		SetGlowEnabled(true);
		SetBoardTransitionEnabled(true);
		break;

	case EML_BoardLockAction::Release:
		bLockedByPrerequisites = false;
		break;

	case EML_BoardLockAction::None:
		break;
	}
}

void AML_BoardSpawner::RefreshAllBoardLockStates(UWorld* World)
{
	if (!World) return;

	// Any board can be listed as a prerequisite of any other, so a solve/reset is re-evaluated by all
	// of them rather than by a chain the boards would have to know about.
	for (TActorIterator<AML_BoardSpawner> It(World); It; ++It)
	{
		if (AML_BoardSpawner* Board = *It; IsValid(Board))
			Board->RefreshLockState();
	}
}

// ==================== Myceland Board Exits (gamepad) ====================

const FML_BoardExit* AML_BoardSpawner::FindExitForTile(const AML_Tile* PlayerTile) const
{
	if (!IsValid(PlayerTile))
		return nullptr;

	for (const FML_BoardExit& Exit : BoardExits)
	{
		for (const TObjectPtr<AML_Tile>& ExitTile : Exit.ExitTiles)
		{
			if (ExitTile == PlayerTile)
				return &Exit;
		}
	}

	return nullptr;
}

AActor* AML_BoardSpawner::GetActiveExitPlaneForTile(const AML_Tile* PlayerTile) const
{
	const FML_BoardExit* Exit = FindExitForTile(PlayerTile);
	if (!Exit || !IsValid(Exit->ExitPlane))
		return nullptr;

	// Disabled by the designer via SetActorEnableCollision(false) — mirrors how the mouse ground-trace stops
	// detecting it. The ground exit is the only way to leave a board, so a disabled one is unusable everywhere.
	if (!Exit->ExitPlane->GetActorEnableCollision())
		return nullptr;

	return Exit->ExitPlane;
}

#if WITH_EDITOR
void AML_BoardSpawner::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;

	// Only meaningful during PIE, where a live player controller exists to react to the toggle.
	AML_PlayerController* PC = GetWorld() ? Cast<AML_PlayerController>(GetWorld()->GetFirstPlayerController()) : nullptr;
	if (!PC) return;

	if (PropName == GET_MEMBER_NAME_CHECKED(AML_BoardSpawner, bGlowEnabled) && !bGlowEnabled)
		PC->ClearActiveGlow();
	else if (PropName == GET_MEMBER_NAME_CHECKED(AML_BoardSpawner, bBoardTransitionEnabled) && !bBoardTransitionEnabled)
		PC->NotifyBoardTransitionDisabled(this);
}
#endif

void AML_BoardSpawner::BeginPlay()
{
	Super::BeginPlay();

	ensureMsgf(BiomeTileSet, TEXT("BiomeTileSet is not set for board : %s"), *GetName());
	if (!BiomeTileSet) return;

	// At runtime, only initialize tiles that already exist in the level.
	// Never spawn new ones — the designer may have intentionally deleted some tiles.
	UpdateCurrentGrid(false);

	// Join the world-wide grid movement walks on. Right after the grid is built and before any early return
	// below, so every board is walkable whatever its save / PuzzleID state.
	if (UML_HexGridSubsystem* HexGrid = UML_HexGridSubsystem::Get(this))
		HexGrid->RegisterBoard(this);

	// Before the save block below, which returns early for a board without a PuzzleID: the lock reads the
	// save for its PREREQUISITES, so it applies to any board the designer gated, ID or not.
	RefreshLockState();

	// A world board is terrain, not a puzzle: no PuzzleID, nothing saved, no win to listen to. Stop here so
	// it never logs the "no PuzzleID" warning meant for puzzles that forgot theirs.
	if (bIsWorldBoard)
		return;

#if !UE_BUILD_SHIPPING
	if (PuzzleID.IsValid())
	{
		for (TActorIterator<AML_BoardSpawner> It(GetWorld()); It; ++It)
		{
			if (*It != this && (*It)->PuzzleID == PuzzleID)
			{
				UE_LOG(LogTemp, Warning,
					TEXT("[BoardSpawner] Duplicate PuzzleID '%s' on '%s' and '%s' — save data will collide!"),
					*PuzzleID.ToString(), *GetName(), *(*It)->GetName());
			}
		}
	}
#endif

	// ---- Save / Load integration ----
	if (!PuzzleID.IsValid())
	{
#if !UE_BUILD_SHIPPING
		// Loud on purpose: a board without an ID silently opts out of the whole save path — it is never
		// marked solved, so it never reloads as solved and never opens the boards that list it in their
		// Required Puzzles. A level re-saved from a stale editor state has already dropped these once,
		// and the only symptom was a puzzle that would not open two playthroughs later.
		UE_LOG(LogTemp, Warning,
			TEXT("[Save] Board '%s' has no PuzzleID — it will never be saved nor count as solved, and any "
			     "board that requires it stays locked. Set its Puzzle ID in the Details panel (ML- Hex Grid)."),
			*GetName());
#endif
		return;
	}

	UML_SaveSubsystem* SaveSys = GetSaveSubsystem();
	if (!SaveSys) return;

	// Look up the saved record once (pointer into the save object — no copy of the grids).
	const FName PuzzleName = PuzzleID.GetTagName();
	const FML_PuzzleSaveRecord* Record = SaveSys->FindPuzzleRecord(PuzzleName);

	// First time this puzzle is ever seen: capture the authored tile layout as its initial grid.
	// Only snapshot when there's no record yet — EnsureInitialGridSaved never overwrites an
	// existing one, so building the snapshot on every subsequent load was wasted work.
	if (!Record)
	{
		SaveSys->EnsureInitialGridSaved(PuzzleName, SnapshotGrid());
		Record = SaveSys->FindPuzzleRecord(PuzzleName);
	}

	// If this puzzle was already solved, restore the solved grid so the player sees it.
	// Subclasses that own their own restore (e.g. the hub) opt out via ShouldAutoRestoreSolvedGrid().
	if (ShouldAutoRestoreSolvedGrid() && Record && Record->bIsSolved && Record->SolvedGrid.Num() > 0)
	{
		int32 Applied = 0;
		for (const FML_TileSaveEntry& Entry : Record->SolvedGrid)
		{
			if (AML_Tile* Tile = GridMap.FindRef(Entry.Axial))
			{
				Tile->UpdateClassAtRuntime_Silent(Entry.TileType,
					BiomeTileSet->GetClassFromTileType(Entry.TileType));
				++Applied;
			}
		}
		bIsPuzzleSolved = true;

		// A won puzzle has its entry/exit (tile-by-tile) system turned off so the player can't
		// re-enter and replay it. That disable happened at win time but isn't part of the saved
		// grid, so replay it here on load — otherwise a completed board loads back as enterable.
		SetBoardTransitionEnabled(false);

		// Likewise turn off the hover preview (cursor/path glow), which IsGlowEnabled() gates, so
		// a completed board doesn't glow under the cursor on load.
		SetGlowEnabled(false);

		if (IsValid(AssociatedObstacle))
		{
			AssociatedObstacle->SetActorHiddenInGame(true);
			AssociatedObstacle->SetActorEnableCollision(false);
		}

		// Replay the water-bridge activation that ran when the puzzle was won, so the
		// bridges are visible/collidable again on load. Deferred one tick so every
		// WaterNavPath actor has finished its own BeginPlay before we reveal it.
		TArray<AActor*> PathsToSpawn = AssociatedWaterPaths;
		GetWorld()->GetTimerManager().SetTimerForNextTick([PathsToSpawn]()
		{
			for (AActor* PathActor : PathsToSpawn)
			{
				if (AML_WaterNavPath* NavPath = Cast<AML_WaterNavPath>(PathActor))
					NavPath->Spawn();
			}
		});

		// NOTE: the win cinematic is NOT replayed here. Playing it in BeginPlay raced the player
		// teleport (both deferred one tick), so the fast-forwarded sequence could finish — firing
		// OnCinematicFinished, which BP_ProgressionManager handles by reading the player's
		// CurrentTileOn — before the player was ever placed, giving "Accessed None ... CurrentTileOn".
		// The replay is now driven from AML_PlayerCharacter::ApplySavedSpawnPosition, AFTER the
		// player is teleported onto its board and CurrentTileOn is updated. See PlayAssociatedWinCinematic.

		UE_LOG(LogTemp, Log, TEXT("[Save] Puzzle '%s' — solved state loaded (%d/%d tiles restored)."),
			*PuzzleID.ToString(), Applied, Record->SolvedGrid.Num());
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("[Save] Puzzle '%s' — not yet solved, loading fresh."),
			*PuzzleID.ToString());
	}

	// Subscribe to the settled-path event (fires one tick after OnWin, once BOTH
	// ClearWinPath passes have run) so the snapshot captures the fully-settled
	// winning path — otherwise the deferred Entry→Exit tiles would still be Water.
	if (UML_WinLoseSubsystem* WinLose = GetWorld()->GetSubsystem<UML_WinLoseSubsystem>())
	{
		WinLose->OnWinPathSettled.AddDynamic(this, &AML_BoardSpawner::HandlePuzzleWon);

		// Reveal the already-connected goals on load. Deferred one tick so the tile
		// Blueprints that draw the links have finished BeginPlay and bound to
		// OnConnectedGoalPathTile — a synchronous broadcast here would fire before they
		// bind. bImmediate=true then broadcasts all at once instead of staggering through
		// the shared queue/timer, which the player spawning (OnBoardChanged →
		// ResetConnectedGoalPathState) would otherwise wipe mid-reveal — cutting off
		// boards with many goals whose queues haven't drained yet.
		TWeakObjectPtr<AML_BoardSpawner> WeakThis(this);
		TWeakObjectPtr<UML_WinLoseSubsystem> WeakWinLose(WinLose);
		GetWorld()->GetTimerManager().SetTimerForNextTick([WeakThis, WeakWinLose]()
		{
			AML_BoardSpawner* Board = WeakThis.Get();
			UML_WinLoseSubsystem* WinLoseSys = WeakWinLose.Get();
			if (Board && WinLoseSys)
				WinLoseSys->TriggerConnectedGoalAnimationForBoard(Board, /*bImmediate=*/true);
		});
	}
}

void AML_BoardSpawner::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UML_HexGridSubsystem* HexGrid = UML_HexGridSubsystem::Get(this))
		HexGrid->UnregisterBoard(this);

	Super::EndPlay(EndPlayReason);
}

void AML_BoardSpawner::UpdateCurrentGridEditor()
{
	// Never spawn new tiles — the designer may have intentionally deleted some.
	// Use "Clear & Rebuild Grid" to regenerate the full procedural shape.
	UpdateCurrentGrid(false);
}

void AML_BoardSpawner::RebuildGrid()
{
	ClearTiles();

	switch (GridLayout)
	{
	case EML_HexGridLayout::HexagonRadius: SpawnHexagonRadius(); break;
	case EML_HexGridLayout::RectangleWH:   SpawnRectangleWH();   break;
	}

	RefreshTileCaches();
}

void AML_BoardSpawner::UpdateCurrentGrid(bool bAllowSpawn)
{
	UWorld* World = GetWorld();
	if (!World) return;

	// Rebuild map from existing tiles owned by this spawner.
	GridMap.Empty();
	SpawnedTiles.Empty();

	for (TActorIterator<AML_Tile> It(World); It; ++It)
	{
		AML_Tile* Tile = *It;
		if (!IsValid(Tile)) continue;
		if (Tile->GetOwner() != this) continue;

		FIntPoint Axial = Tile->GetAxialCoord();
		if (Axial == FIntPoint(0, 0))
		{
			const FIntPoint Derived = WorldToAxial(Tile->GetActorLocation());
			if (Derived != Axial)
			{
				Axial = Derived;
				Tile->SetAxialCoord(Axial);
			}
		}

		if (GridMap.Contains(Axial))
		{
			const FIntPoint Derived = WorldToAxial(Tile->GetActorLocation());
			if (!GridMap.Contains(Derived))
			{
				Axial = Derived;
				Tile->SetAxialCoord(Axial);
			}
		}

		if (GridMap.Contains(Axial))
		{
			Tile->Destroy();
			continue;
		}

		GridMap.Add(Axial, Tile);
		SpawnedTiles.Add(Tile);
	}

	// Determine all desired axial coordinates
	TSet<FIntPoint> DesiredAxials;
	DesiredAxials.Reserve(GridLayout == EML_HexGridLayout::HexagonRadius
		? (1 + 3 * Radius * (Radius + 1))
		: (GridWidth * GridHeight));

	switch (GridLayout)
	{
	case EML_HexGridLayout::HexagonRadius:
		{
			for (int32 Q = -Radius; Q <= Radius; ++Q)
			{
				const int32 RMin = FMath::Max(-Radius, -Q - Radius);
				const int32 RMax = FMath::Min(Radius, -Q + Radius);
				for (int32 R = RMin; R <= RMax; ++R)
				{
					DesiredAxials.Add(FIntPoint(Q, R));
				}
			}
			break;
		}
	case EML_HexGridLayout::RectangleWH:
		{
			for (int32 Row = 0; Row < GridHeight; ++Row)
			{
				for (int32 Col = 0; Col < GridWidth; ++Col)
				{
					DesiredAxials.Add(OffsetToAxial(Col, Row));
				}
			}
			break;
		}
	}

	// Remove tiles that are no longer part of the desired grid
	for (const TPair<FIntPoint, TObjectPtr<AML_Tile>>& Pair : GridMap)
	{
		if (DesiredAxials.Contains(Pair.Key)) continue;
		if (Pair.Value) Pair.Value->Destroy();
	}

	// Spawn new tiles or reattach existing
	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	TMap<FIntPoint, TObjectPtr<AML_Tile>> NewTilesByAxial;
	NewTilesByAxial.Reserve(DesiredAxials.Num());

	for (const FIntPoint& Axial : DesiredAxials)
	{
		AML_Tile* Tile = nullptr;
		
		// Tile exists in the level → reattach and reinitialize it
		if (const TObjectPtr<AML_Tile>* Found = GridMap.Find(Axial))
		{
			Tile = Found->Get();
			if (!Tile) continue;

			// Reattach to the board spawner without changing scale/rotation
			Tile->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
			Tile->SetAxialCoord(Axial);

			Tile->Initialize(BiomeTileSet);
		}
		// Tile was deleted by the designer — only spawn if explicitly allowed (editor operations)
		else if (bAllowSpawn && TileClass)
		{
			const FVector Location = AxialToWorld(Axial.X, Axial.Y);
			const FTransform SpawnTransform(FRotator::ZeroRotator, Location, TileScale);
			Tile = World->SpawnActor<AML_Tile>(TileClass, SpawnTransform, Params);
			if (!Tile) continue;

			Tile->AttachToActor(this, FAttachmentTransformRules::KeepRelativeTransform);
			Tile->SetAxialCoord(Axial);
			
			Tile->Initialize(BiomeTileSet);
		}

		if (Tile) NewTilesByAxial.Add(Axial, Tile);
	}

	GridMap = MoveTemp(NewTilesByAxial);
	RefreshTileCaches();
	ValidateGridAlignment();
}

void AML_BoardSpawner::RefreshTileCaches()
{
	SpawnedTiles.Empty();
	SpawnedTiles.Reserve(GridMap.Num());
	TreeTiles.Empty();

	for (const TPair<FIntPoint, TObjectPtr<AML_Tile>>& Pair : GridMap)
	{
		AML_Tile* Tile = Pair.Value.Get();
		if (!IsValid(Tile))
			continue;

		SpawnedTiles.Add(Tile);
		Tile->SetBorderTile(false);
		Tile->RefreshHeightLevelFromLocation();

		if (UML_TileTypeTraits::IsTreeType(Tile->GetCurrentType()))
		{
			TreeTiles.Add(Tile);
		}

		for (const FIntPoint& Dir : Directions)
		{
			const FIntPoint NeighborAxial = Pair.Key + Dir;
			if (!GridMap.Contains(NeighborAxial))
			{
				Tile->SetBorderTile(true);
				break;
			}
		}
	}
}

void AML_BoardSpawner::ClearTiles()
{
	UWorld* World = GetWorld();
	if (!World) return;

	// Destroy all tiles where owner=this
	for (TActorIterator<AML_Tile> It(World); It; ++It)
	{
		AML_Tile* Tile = *It;
		if (!IsValid(Tile)) continue;

		if (Tile->GetOwner() == this)
		{
			// Destroy attached orphan visuals first; otherwise destroying the tile only
			// detaches them and leaves them floating in the level forever.
			Tile->DestroyStaleChildActors();
			Tile->Destroy();
		}
	}

	SpawnedTiles.Empty();
	TreeTiles.Empty();
	GridMap.Empty();
}

void AML_BoardSpawner::DestroyStaleTiles() const
{
	for (TActorIterator<AML_Tile> It(GetWorld()); It; ++It)
	{
		AML_Tile* Tile = *It;
		if (!IsValid(Tile)) continue;
		if (Tile->GetOwner() != this) continue;
		Tile->DestroyStaleChildActors();
	}
}

// Blueprint-only
TArray<AML_Tile*> AML_BoardSpawner::GetNeighbors(AML_Tile* CenterTile) const
{
	FML_TileNeighbors Neighbors;
	GetNeighbors(CenterTile, Neighbors);
	return TArray<AML_Tile*>(Neighbors);
}

void AML_BoardSpawner::GetNeighbors(const AML_Tile* CenterTile, FML_TileNeighbors& OutNeighbors) const
{
	OutNeighbors.Reset();
	if (!CenterTile) return;

	const FIntPoint CenterAxial = CenterTile->GetAxialCoord();

	for (const FIntPoint& Dir : Directions)
	{
		const TObjectPtr<AML_Tile>* Found = GridMap.Find(CenterAxial + Dir);
		OutNeighbors.Add(Found ? Found->Get() : nullptr);
	}
}

TMap<FIntPoint, AML_Tile*> AML_BoardSpawner::GetGridMap() const
{
	TMap<FIntPoint, AML_Tile*> Result;
	Result.Reserve(GridMap.Num());
	for (const TPair<FIntPoint, TObjectPtr<AML_Tile>>& Pair : GridMap)
	{
		Result.Add(Pair.Key, Pair.Value.Get());
	}
	return Result;
}

const TMap<FIntPoint, AML_Tile*>& AML_BoardSpawner::GetGridMapRef() const
{
	if (!bGridMapCacheBuilt)
	{
		GridMapCache.Reserve(GridMap.Num());
		for (const TPair<FIntPoint, TObjectPtr<AML_Tile>>& Pair : GridMap)
			GridMapCache.Add(Pair.Key, Pair.Value.Get());
		bGridMapCacheBuilt = true;
	}
	return GridMapCache;
}

TArray<AML_Tile*> AML_BoardSpawner::GetGridTiles()
{
	TArray<AML_Tile*> Result;
	Result.Reserve(GridMap.Num());
	for (const TPair<FIntPoint, TObjectPtr<AML_Tile>>& Pair : GridMap)
	{
		Result.Add(Pair.Value.Get());
	}
	return Result;
}

TArray<AML_Tile*> AML_BoardSpawner::GetTreeTiles() const
{
	TArray<AML_Tile*> Result;
	Result.Reserve(TreeTiles.Num());
	for (const TObjectPtr<AML_Tile>& Tile : TreeTiles)
	{
		Result.Add(Tile.Get());
	}
	return Result;
}

AML_Tile* AML_BoardSpawner::FindClosestWalkableBorderTile(const FVector& WorldLocation) const
{
	float MinDistSq = FLT_MAX;
	AML_Tile* ClosestBorderTile = nullptr;

	for (const TPair<FIntPoint, TObjectPtr<AML_Tile>>& Pair : GridMap)
	{
		AML_Tile* Tile = Pair.Value.Get();
		if (!IsValid(Tile) || !UML_HexPathfinder::IsTileWalkable(Tile))
			continue;

		// Check if it's a border tile (doesn't have all 6 of its neighbors)
		// If it's not a border tile, continue
		if (!Tile->IsBorderTile())
			continue;

		float DistSq = FVector::DistSquared(WorldLocation, Tile->GetActorLocation());
		if (DistSq < MinDistSq)
		{
			MinDistSq = DistSq;
			ClosestBorderTile = Tile;
		}
	}

	return ClosestBorderTile;
}

AML_CameraRail* AML_BoardSpawner::GetClosestCameraRail(const FVector& WorldLocation) const
{
	float MinDistSq = FLT_MAX;
	AML_CameraRail* ClosestCameraRail = nullptr;

	// A board with no rail of its own (a test map, a board placed before its rails) falls back to every rail
	// of the level, so leaving it never ends on a null view target (Unreal then shows the pawn's own view).
	TArray<AML_CameraRail*> CandidateRails;
	for (AML_CameraRail* Rail : AssociatedCameraRails)
		if (IsValid(Rail))
			CandidateRails.Add(Rail);

	if (CandidateRails.IsEmpty())
		for (TActorIterator<AML_CameraRail> It(GetWorld()); It; ++It)
			CandidateRails.Add(*It);

	for (AML_CameraRail* Rail : CandidateRails)
	{
		// Get the closest key from the LooAt spline from the WorldLocation parameter, then get the world location of this key
		float InputKey = Rail->GetLookAtFromCameraRail()->FindInputKeyClosestToWorldLocation(WorldLocation);
		FVector LookAtLocation = Rail->GetLookAtFromCameraRail()->GetLocationAtSplineInputKey(InputKey, ESplineCoordinateSpace::World);
		
		float Dist = FVector::DistSquared(WorldLocation, LookAtLocation);
		if (Dist < MinDistSq)
		{
			MinDistSq = Dist;
			ClosestCameraRail = Rail;
		}
	}
	
	return ClosestCameraRail;
}

FVector AML_BoardSpawner::AxialToWorld(int32 Q, int32 R) const
{
	const FVector2D Local = AxialToLocal2D(Q, R);
	return GetActorLocation() + FVector(Local.X, Local.Y, 0.f);
}

FIntPoint AML_BoardSpawner::WorldToAxial(const FVector& WorldLocation) const
{
	return Local2DToAxial(FVector2D(WorldLocation - GetActorLocation()));
}

FVector2D AML_BoardSpawner::AxialToLocal2D(int32 Q, int32 R)
{
	const UML_MycelandDeveloperSettings* Settings = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();
	const float TileSize = FMath::Max(Settings->HexTileSize, 1.f);
	const float Sqrt3 = 1.73205080757f;

	if (Settings->HexOrientation == EML_HexOrientation::FlatTop)
		return FVector2D(TileSize * (1.5f * Q), TileSize * (Sqrt3 * (R + 0.5f * Q)));

	// PointyTop
	return FVector2D(TileSize * (Sqrt3 * (Q + 0.5f * R)), TileSize * (1.5f * R));
}

FVector AML_BoardSpawner::SnapToLattice(const FVector& WorldLocation)
{
	const FVector& Origin = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings()->HexGridOrigin;
	const FIntPoint Axial = Local2DToAxial(FVector2D(WorldLocation - Origin));
	const FVector2D Snapped = AxialToLocal2D(Axial.X, Axial.Y);
	return FVector(Origin.X + Snapped.X, Origin.Y + Snapped.Y, WorldLocation.Z);
}

FIntPoint AML_BoardSpawner::Local2DToAxial(const FVector2D& Local)
{
	const UML_MycelandDeveloperSettings* Settings = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();
	const float TileSize = FMath::Max(Settings->HexTileSize, 1.f);
	const float Sqrt3 = 1.73205080757f;

	float Qf = 0.f;
	float Rf = 0.f;

	if (Settings->HexOrientation == EML_HexOrientation::FlatTop)
	{
		Qf = (2.f / 3.f * Local.X) / TileSize;
		Rf = (-1.f / 3.f * Local.X + (Sqrt3 / 3.f) * Local.Y) / TileSize;
	}
	else
	{
		Qf = ((Sqrt3 / 3.f) * Local.X - (1.f / 3.f) * Local.Y) / TileSize;
		Rf = (2.f / 3.f * Local.Y) / TileSize;
	}

	const float Xf = Qf;
	const float Zf = Rf;
	const float Yf = -Xf - Zf;

	int32 Rx = FMath::RoundToInt(Xf);
	int32 Ry = FMath::RoundToInt(Yf);
	int32 Rz = FMath::RoundToInt(Zf);

	const float XDiff = FMath::Abs(Rx - Xf);
	const float YDiff = FMath::Abs(Ry - Yf);
	const float ZDiff = FMath::Abs(Rz - Zf);

	if (XDiff > YDiff && XDiff > ZDiff)
	{
		Rx = -Ry - Rz;
	}
	else if (YDiff > ZDiff)
	{
		Ry = -Rx - Rz;
	}
	else
	{
		Rz = -Rx - Ry;
	}

	return FIntPoint(Rx, Rz);
}

// ==================== Global Lattice & Heights ====================

void AML_BoardSpawner::SnapBoardToLattice()
{
	const FVector Current = GetActorLocation();
	const FVector Snapped = SnapToLattice(Current);
	if (FVector::DistSquared2D(Current, Snapped) <= FMath::Square(0.1f))
		return;

	// Tiles are attached to the board, so they follow it.
	Modify();
	SetActorLocation(Snapped);

	UE_LOG(LogTemp, Log, TEXT("[HexGrid] Board '%s' snapped to the global lattice (moved %.1f cm)."),
		*GetName(), FVector::Dist2D(Current, Snapped));
}

void AML_BoardSpawner::SnapTileHeights()
{
	const UML_MycelandDeveloperSettings* Settings = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();

	int32 Moved = 0;
	for (const TPair<FIntPoint, TObjectPtr<AML_Tile>>& Pair : GridMap)
	{
		AML_Tile* Tile = Pair.Value.Get();
		if (!IsValid(Tile) || Tile->GetHeightLevelError() <= 0.1f)
			continue;

		FVector Location = Tile->GetActorLocation();
		Location.Z = Settings->GetZFromHeightLevel(Settings->GetHeightLevelFromZ(Location.Z));

		Tile->Modify();
		Tile->SetActorLocation(Location);
		++Moved;
	}

	RefreshTileCaches();

	UE_LOG(LogTemp, Log, TEXT("[HexGrid] Board '%s': %d tile(s) snapped to their height level."), *GetName(), Moved);
}

void AML_BoardSpawner::ValidateGridAlignment() const
{
#if !UE_BUILD_SHIPPING
	// 1 cm of slack: hand-placed boards are never exact to the float, but a real offset is always far more.
	constexpr float Tolerance = 1.f;

	const FVector Location = GetActorLocation();
	const float LatticeOffset = FVector::Dist2D(Location, SnapToLattice(Location));
	if (LatticeOffset > Tolerance)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[HexGrid] Board '%s' is %.1f cm off the global hex lattice — the player will not be able to walk "
			     "between its tiles and the neighboring boards. Click 'Snap Board To Lattice' (ML- Hex Grid)."),
			*GetName(), LatticeOffset);
	}

	int32 OffLevelTiles = 0;
	for (const TPair<FIntPoint, TObjectPtr<AML_Tile>>& Pair : GridMap)
	{
		if (const AML_Tile* Tile = Pair.Value.Get(); IsValid(Tile) && Tile->GetHeightLevelError() > Tolerance)
			++OffLevelTiles;
	}

	if (OffLevelTiles > 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[HexGrid] Board '%s' has %d tile(s) between two height levels — their level is rounded, so the "
			     "climb rules may not match what you see. Click 'Snap Tile Heights' (ML- Hex Grid)."),
			*GetName(), OffLevelTiles);
	}
#endif
}

void AML_BoardSpawner::SpawnHexagonRadius()
{
	if (!TileClass) return;
	UWorld* World = GetWorld();
	if (!World) return;

	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	for (int32 Q = -Radius; Q <= Radius; ++Q)
	{
		const int32 RMin = FMath::Max(-Radius, -Q - Radius);
		const int32 RMax = FMath::Min( Radius, -Q + Radius);

		for (int32 R = RMin; R <= RMax; ++R)
		{
			const FVector Location = AxialToWorld(Q, R);
			const FTransform TileTransform(TileRotation, Location, TileScale);

			AML_Tile* Tile = World->SpawnActor<AML_Tile>(TileClass, TileTransform, Params);
			if (!Tile) continue;
			Tile->Initialize(BiomeTileSet);

			SpawnedTiles.Add(Tile);
			Tile->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
			const FIntPoint Axial(Q, R);
			Tile->SetAxialCoord(Axial);
			GridMap.Add(Axial, Tile);
		}
	}
}

FIntPoint AML_BoardSpawner::OffsetToAxial(int32 Col, int32 Row) const
{
	switch (OffsetLayout)
	{
	case EML_HexOffsetLayout::OddR:
		{
			const int32 Q = Col - ((Row - (Row & 1)) / 2);
			return FIntPoint(Q, Row);
		}
	case EML_HexOffsetLayout::EvenR:
		{
			const int32 Q = Col - ((Row + (Row & 1)) / 2);
			return FIntPoint(Q, Row);
		}
	case EML_HexOffsetLayout::OddQ:
		{
			const int32 R = Row - ((Col - (Col & 1)) / 2);
			return FIntPoint(Col, R);
		}
	case EML_HexOffsetLayout::EvenQ:
	default:
		{
			const int32 R = Row - ((Col + (Col & 1)) / 2);
			return FIntPoint(Col, R);
		}
	}
}

void AML_BoardSpawner::SpawnRectangleWH()
{
	if (!TileClass) return;
	UWorld* World = GetWorld();
	if (!World) return;

	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	for (int32 Row = 0; Row < GridHeight; ++Row)
	{
		for (int32 Col = 0; Col < GridWidth; ++Col)
		{
			const FIntPoint Axial = OffsetToAxial(Col, Row);
			const int32 Q = Axial.X;
			const int32 R = Axial.Y;

			const FVector Location = AxialToWorld(Q, R);
			const FTransform TileTransform(TileRotation, Location, TileScale);

			AML_Tile* Tile = World->SpawnActor<AML_Tile>(TileClass, TileTransform, Params);
			if (!Tile) continue;
			Tile->Initialize(BiomeTileSet);

			SpawnedTiles.Add(Tile);
			Tile->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
			const FIntPoint AxialQR(Q, R);
			Tile->SetAxialCoord(AxialQR);
			GridMap.Add(AxialQR, Tile);
		}
	}
}

// ==================== Save / Load ====================

TArray<FML_TileSaveEntry> AML_BoardSpawner::SnapshotGrid() const
{
	TArray<FML_TileSaveEntry> Entries;
	Entries.Reserve(GridMap.Num());
	for (const TPair<FIntPoint, TObjectPtr<AML_Tile>>& Pair : GridMap)
	{
		if (!Pair.Value) continue;
		FML_TileSaveEntry Entry;
		Entry.Axial    = Pair.Key;
		Entry.TileType = Pair.Value->GetCurrentType();                                                                                                                                                                
		Entries.Add(Entry);
	}
	return Entries;
}

UML_SaveSubsystem* AML_BoardSpawner::GetSaveSubsystem() const
{
	const UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UML_SaveSubsystem>() : nullptr;
}

FName AML_BoardSpawner::GetLevelKey() const
{
	// Strip the "UEDPIE_N_" prefix that Unreal prepends in PIE so the key is identical
	// whether the puzzle is solved in the editor or in a packaged build.
	FString MapName = GetWorld()->GetMapName();
	MapName.RemoveFromStart(GetWorld()->StreamingLevelsPrefix);
	return FName(*MapName);
}

void AML_BoardSpawner::HandlePuzzleWon()
{
	// Only save for the board that actually triggered the win.
	if (const UML_WinLoseSubsystem* WinLose = GetWorld()->GetSubsystem<UML_WinLoseSubsystem>())
	{
		if (WinLose->CurrentBoardSpawner != this) return;
	}

	if (!PuzzleID.IsValid()) return;
	UML_SaveSubsystem* SaveSys = GetSaveSubsystem();
	if (!SaveSys) return;

	SaveSys->MarkPuzzleSolved(PuzzleID.GetTagName(), SnapshotGrid(), GetLevelKey());

	// This board is only now recorded as solved, so run the pass from here rather than from each board's
	// own win handler: every board sees the new save state, whatever order they were bound in.
	RefreshAllBoardLockStates(GetWorld());
}

void AML_BoardSpawner::DebugAutoWin()
{
#if WITH_EDITOR
	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Debug] Debug Auto Win only works during Play (PIE)."));
		return;
	}

	if (UML_WinLoseSubsystem* WinLose = World->GetSubsystem<UML_WinLoseSubsystem>())
		WinLose->ForceWinBoard(this);
#endif
}

void AML_BoardSpawner::PlayAssociatedWinCinematic() const
{
	if (!AssociatedWinCinematic) return;

	if (UML_CinematicSubsystem* Cinematic = GetWorld()->GetSubsystem<UML_CinematicSubsystem>())
	{
		// On load we don't want the player to sit through the full win cinematic — fast-forward
		// it hard so it resolves near-instantly while still firing all its Sequencer events.
		// bQueueIfBusy: several already-solved boards each request this on the same load tick,
		// and only one cinematic can play at a time, so they queue and drain one after another.
		// bRecenterCameraWhenDone: once the whole queue finishes, snap the camera back onto the
		// player's board camera (the fast-forwarded cinematics leave it parked elsewhere). Only the
		// on-load path sets this, so a normal win cinematic finishing in-game never moves the camera.
		constexpr float FastForwardRate = 50.f;
		Cinematic->PlayCinematic(AssociatedWinCinematic,
			/*BlendCamera=*/nullptr, /*BlendTime=*/0.f, /*ReturnBlendTime=*/0.f,
			/*bWaitForBlendToFinish=*/true, /*PlayerController=*/nullptr,
			/*PlayRate=*/FastForwardRate, /*bQueueIfBusy=*/true,
			/*bRecenterCameraWhenDone=*/true);
	}
}

void AML_BoardSpawner::RestoreToInitialState()
{
	UML_SaveSubsystem* SaveSys = GetSaveSubsystem();
	if (!SaveSys || !PuzzleID.IsValid()) return;

	// InitialGrid is preserved by ResetPuzzle (only bIsSolved and SolvedGrid are cleared),
	// so it is always safe to read here whether called before or after the save cascade.
	const FML_PuzzleSaveRecord* Record = SaveSys->FindPuzzleRecord(PuzzleID.GetTagName());
	if (!Record || Record->InitialGrid.IsEmpty()) return;

	for (const FML_TileSaveEntry& Entry : Record->InitialGrid)
	{
		if (AML_Tile* Tile = GridMap.FindRef(Entry.Axial))
		{
			Tile->UpdateClassAtRuntime_Silent(Entry.TileType,
				BiomeTileSet->GetClassFromTileType(Entry.TileType));
		}
	}

	bIsPuzzleSolved = false;
	bGridMapCacheBuilt = false;

	if (IsValid(AssociatedObstacle))
	{
		AssociatedObstacle->SetActorHiddenInGame(false);
		AssociatedObstacle->SetActorEnableCollision(true);
	}

	// Undo the water-bridge activation so replaying the puzzle starts from a clean board.
	for (AActor* PathActor : AssociatedWaterPaths)
	{
		if (AML_WaterNavPath* NavPath = Cast<AML_WaterNavPath>(PathActor))
			NavPath->Despawn();
	}

	// Erase the revealed goal-link visuals — the board is blank again, so the connections
	// shown on load (via TriggerConnectedGoalAnimationForBoard) must be cleared too.
	if (UML_WinLoseSubsystem* WinLose = GetWorld()->GetSubsystem<UML_WinLoseSubsystem>())
		WinLose->ResetConnectedGoalAnimationForBoard(this);

	RefreshTileCaches();
}

void AML_BoardSpawner::ReplayPuzzle()
{
	if (!PuzzleID.IsValid()) return;

	UML_SaveSubsystem* SaveSys = GetSaveSubsystem();
	if (!SaveSys) return;

	// Cascade the save reset within this level only. ResetPuzzle returns every puzzle ID
	// that was cleared (this one + all solved after it in this level).
	const TArray<FName> ResetIDs = SaveSys->ResetPuzzle(PuzzleID.GetTagName(), GetLevelKey());
	if (ResetIDs.IsEmpty()) return;

	UWorld* World = GetWorld();
	if (!World) return;

	// Restore every affected board to its initial tile state.
	for (TActorIterator<AML_BoardSpawner> It(World); It; ++It)
	{
		AML_BoardSpawner* Board = *It;
		if (!IsValid(Board) || !Board->PuzzleID.IsValid()) continue;
		if (ResetIDs.Contains(Board->PuzzleID.GetTagName()))
			Board->RestoreToInitialState();
	}

	// The cascade just un-solved this puzzle and everything after it, so boards gated on any of them
	// close back — otherwise a reset would leave the rest of the level open behind it.
	RefreshAllBoardLockStates(World);
}

void AML_BoardSpawner::AnalyzeCurrentPuzzle()
{
	UpdateCurrentGrid(false);

	const FML_PuzzleState State = BuildPuzzleStateFromCurrentGrid();
	const FML_PuzzleSolveReport Report = FML_PuzzleSolver::Solve(State, PuzzleGenerationSettings);

	UE_LOG(LogTemp, Error, TEXT("Puzzle analysis: Solvable=%s, Solutions=%d, Shortest=%d"),
		Report.bSolvable ? TEXT("true") : TEXT("false"),
		Report.SolutionCount,
		Report.ShortestSolutionLength);

	for (const FIntPoint& Action : Report.ShortestSolution.PlantActions)
	{
		UE_LOG(LogTemp, Error, TEXT("Solution action: Plant at Q=%d R=%d"), Action.X, Action.Y);
	}
}

FML_PuzzleState AML_BoardSpawner::BuildPuzzleStateFromCurrentGrid() const
{
	FML_PuzzleState State;
	State.Energy = EnergyForPuzzle;

	for (const TPair<FIntPoint, TObjectPtr<AML_Tile>>& Pair : GridMap)
	{
		const AML_Tile* Tile = Pair.Value.Get();
		if (!IsValid(Tile))
		{
			continue;
		}

		FML_PuzzleCell Cell;
		Cell.Axial = Pair.Key;
		Cell.Type = Tile->GetCurrentType();
		Cell.bHasCollectible = Tile->HasCollectible();

		State.Cells.Add(Cell);
	}

	return State;
}
 
