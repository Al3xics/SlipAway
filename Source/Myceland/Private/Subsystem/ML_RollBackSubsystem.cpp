// Copyright Myceland Team, All Rights Reserved.

#include "Subsystem/ML_RollBackSubsystem.h"

#include "Algo/Reverse.h"
#include "HAL/PlatformTime.h"
#include "Audio/ML_FMODEvents.h"
#include "Collectible/ML_Collectible.h"
#include "Components/PrimitiveComponent.h"
#include "Data Asset/ML_BiomeTileSet.h"
#include "Developer Settings/ML_MycelandDeveloperSettings.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Player/ML_PlayerCharacter.h"
#include "Player/ML_PlayerController.h"
#include "Subsystem/ML_BoardActionSubsystem.h"
#include "Subsystem/ML_WavePropagationSubsystem.h"
#include "Subsystem/ML_SoundSubsystem.h"
#include "Tiles/ML_BoardSpawner.h"
#include "Tiles/ML_Tile.h"

void UML_RollBackSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bUndoGroupInProgress)
		ContinueUndoGroupSlice();
}

bool UML_RollBackSubsystem::IsTickable() const
{
	return bUndoGroupInProgress;
}

TStatId UML_RollBackSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UML_RollBackSubsystem, STATGROUP_Tickables);
}

double UML_RollBackSubsystem::MakeUndoSliceDeadline() const
{
	const float BudgetMs = DevSettings ? DevSettings->RollbackFrameBudgetMs : 8.f;
	return FPlatformTime::Seconds() + static_cast<double>(BudgetMs) / 1000.0;
}

void UML_RollBackSubsystem::EnsureInitialized()
{
	if (!GetWorld()) return;

	PlayerController = Cast<AML_PlayerController>(GetWorld()->GetFirstPlayerController());
	PlayerCharacter = Cast<AML_PlayerCharacter>(PlayerController->GetPawn());
	DevSettings = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();
	
	if (!bBoardChangedDelegateBound)
	{
		// Puzzles only: leaving a puzzle onto a world board resets it like leaving onto open ground, and
		// leaving a world board never "resets" it.
		PlayerCharacter->OnPuzzleBoardChanged.AddDynamic(this, &UML_RollBackSubsystem::OnBoardChanged);
		bBoardChangedDelegateBound = true;
	}

	ensure(PlayerController);
}

TArray<FML_ActionUndoRecord>* UML_RollBackSubsystem::GetCurrentBoardStack()
{
	if (!PlayerController) return nullptr;

	AML_PlayerCharacter* PC = Cast<AML_PlayerCharacter>(PlayerController->GetPawn());
	if (!PC || !PC->CurrentTileOn) return nullptr;

	AML_BoardSpawner* Board = PC->CurrentTileOn->GetBoardSpawnerFromTile();
	if (!IsValid(Board)) return nullptr;

	return &BoardUndoStacks.FindOrAdd(Board);
}

void UML_RollBackSubsystem::BeginTurnRecord(AML_Tile* OriginTile)
{
	if (!PlayerController) return;

	AML_PlayerCharacter* PC = Cast<AML_PlayerCharacter>(PlayerController->GetPawn());
	if (!PC || !PC->CurrentTileOn) return;

	bHasActiveTurnRecord = true;
	CurrentTurnRecord = FML_TurnUndoRecord{};
	CurrentTurnRecord.EnergyBefore = PlayerController->EnergyComponent->GetCurrentEnergy() + 1;
	CurrentTurnRecord.PlayerAxialBefore = PC->CurrentTileOn->GetAxialCoord();
	CurrentTurnRecord.PlayerWorldBefore = PC->GetActorLocation();
	CurrentTurnRecord.OriginTile = OriginTile;

	UndoSequenceCounter = 0;
}

void UML_RollBackSubsystem::CommitTurnRecord()
{
	if (!bHasActiveTurnRecord) return;

	FML_ActionUndoRecord Action;
	Action.Type = EML_UndoActionType::PlantWaves;
	Action.Turn = CurrentTurnRecord;

	if (TArray<FML_ActionUndoRecord>* Stack = GetCurrentBoardStack())
	{
		Stack->Add(Action);
	}

	bHasActiveTurnRecord = false;
	CurrentTurnRecord = FML_TurnUndoRecord{};
}

void UML_RollBackSubsystem::DiscardTurnRecord()
{
	bHasActiveTurnRecord = false;
	CurrentTurnRecord = FML_TurnUndoRecord{};
}

void UML_RollBackSubsystem::RecordTileForUndo(AML_Tile* Tile, int32 DistanceFromOrigin, int32 PriorityIndex)
{
	if (!bHasActiveTurnRecord || !IsValid(Tile)) return;

	FML_TileUndoDelta Delta;
	Delta.Tile = Tile;
	Delta.OldType = Tile->GetCurrentType();
	Delta.bOldConsumedGrass = Tile->bConsumedGrass;
	Delta.bOldHasCollectible = Tile->HasCollectible();
	Delta.PriorityIndex = PriorityIndex;
	Delta.DistanceFromOrigin = DistanceFromOrigin;
	Delta.Sequence = UndoSequenceCounter++;

	CurrentTurnRecord.TileDeltas.Add(Delta);
}

void UML_RollBackSubsystem::RecordSpawnedActor(AActor* Spawned, int32 DistanceFromOrigin, int32 PriorityIndex)
{
	if (!bHasActiveTurnRecord || !IsValid(Spawned)) return;

	FML_SpawnUndoDelta Delta;
	Delta.SpawnedActor = Spawned;
	if (AML_Collectible* Collectible = Cast<AML_Collectible>(Spawned))
	{
		if (AML_Tile* OwningTile = Collectible->GetOwningTile())
		{
			Delta.bIsCollectible = true;
			Delta.CollectibleAxial = OwningTile->GetAxialCoord();
		}
	}
	Delta.PriorityIndex = PriorityIndex;
	Delta.DistanceFromOrigin = DistanceFromOrigin;
	Delta.Sequence = UndoSequenceCounter++;

	CurrentTurnRecord.SpawnDeltas.Add(Delta);
}

bool UML_RollBackSubsystem::IsCurrentBoardSolved() const
{
	if (!PlayerController) return false;
	const AML_PlayerCharacter* PC = Cast<AML_PlayerCharacter>(PlayerController->GetPawn());
	if (!PC || !PC->CurrentTileOn) return false;
	const AML_BoardSpawner* Board = PC->CurrentTileOn->GetBoardSpawnerFromTile();
	return IsValid(Board) && Board->bIsPuzzleSolved;
}

bool UML_RollBackSubsystem::CanUndo() const
{
	if (bIsUndoAnimating) return false;
	if (IsCurrentBoardSolved()) return false; // no undo once the puzzle is won
	if (!PlayerController) return false;

	const UML_WavePropagationSubsystem* WaveSubsystem = GetWorld() ? GetWorld()->GetSubsystem<UML_WavePropagationSubsystem>() : nullptr;
	if (WaveSubsystem && WaveSubsystem->IsResolvingTiles()) return false;

	AML_PlayerCharacter* PC = Cast<AML_PlayerCharacter>(PlayerController->GetPawn());
	if (!PC || !PC->CurrentTileOn) return false;

	AML_BoardSpawner* Board = PC->CurrentTileOn->GetBoardSpawnerFromTile();
	if (!IsValid(Board)) return false;

	const TArray<FML_ActionUndoRecord>* Stack = BoardUndoStacks.Find(Board);
	return Stack && Stack->Num() > 0;
}

void UML_RollBackSubsystem::OnBoardChanged(const AML_Tile* OldTile, const AML_Tile* NewTile)
{
	const bool bWasNull = (OldTile == nullptr);
	const bool bIsNull  = (NewTile == nullptr);

	if (bWasNull != bIsNull)
	{
		if (bIsNull)
		{
			if (UWorld* World = GetWorld())
			{
				const AML_BoardSpawner* OldBoard = OldTile->GetBoardSpawnerFromTile();
				if (IsValid(OldTile) && IsValid(OldBoard) && !OldBoard->bIsPuzzleSolved)
				{
					if (UML_RollBackSubsystem* RollBackSubsystem = World->GetSubsystem<UML_RollBackSubsystem>())
					{
						RollBackSubsystem->ResetAllActions_ExcludingMoves_Instant(const_cast<AML_BoardSpawner*>(OldBoard));
					}
				}
			}
		}
	}
	// Transition board A → board B
	else if (!bWasNull && !bIsNull)
	{
		const AML_BoardSpawner* OldBoard = OldTile->GetBoardSpawnerFromTile();
		const AML_BoardSpawner* NewBoard = NewTile->GetBoardSpawnerFromTile();

		if (OldBoard != NewBoard)
		{
			if (UWorld* World = GetWorld())
			{
				if (IsValid(OldBoard) && !OldBoard->bIsPuzzleSolved)
				{
					if (UML_RollBackSubsystem* RollBackSubsystem = World->GetSubsystem<UML_RollBackSubsystem>())
					{
						RollBackSubsystem->ResetAllActions_ExcludingMoves_Instant(const_cast<AML_BoardSpawner*>(OldBoard));
					}
				}
			}
		}
	}
}

void UML_RollBackSubsystem::PeekNextUndoGroup(int32& OutPriority, int32& OutDistance) const
{
	OutPriority = 0;
	OutDistance = 0;

	const bool bHasTile = PendingUndoTileIndex < PendingUndoTileDeltas.Num();
	const bool bHasSpawn = PendingUndoSpawnIndex < PendingUndoSpawnDeltas.Num();

	if (bHasTile && !bHasSpawn)
	{
		OutPriority = PendingUndoTileDeltas[PendingUndoTileIndex].PriorityIndex;
		OutDistance = PendingUndoTileDeltas[PendingUndoTileIndex].DistanceFromOrigin;
		return;
	}
	if (!bHasTile && bHasSpawn)
	{
		OutPriority = PendingUndoSpawnDeltas[PendingUndoSpawnIndex].PriorityIndex;
		OutDistance = PendingUndoSpawnDeltas[PendingUndoSpawnIndex].DistanceFromOrigin;
		return;
	}
	if (!bHasTile && !bHasSpawn)
		return;

	const FML_TileUndoDelta& TD = PendingUndoTileDeltas[PendingUndoTileIndex];
	const FML_SpawnUndoDelta& SD = PendingUndoSpawnDeltas[PendingUndoSpawnIndex];

	// Undo replays in reverse: the group with the highest (Priority, Distance) goes first.
	const bool bTileWins = (TD.PriorityIndex != SD.PriorityIndex)
		? TD.PriorityIndex > SD.PriorityIndex
		: TD.DistanceFromOrigin >= SD.DistanceFromOrigin;

	if (bTileWins)
	{
		OutPriority = TD.PriorityIndex;
		OutDistance = TD.DistanceFromOrigin;
	}
	else
	{
		OutPriority = SD.PriorityIndex;
		OutDistance = SD.DistanceFromOrigin;
	}
}

void UML_RollBackSubsystem::RunUndoWave()
{
	if (PendingUndoTileIndex >= PendingUndoTileDeltas.Num()
		&& PendingUndoSpawnIndex >= PendingUndoSpawnDeltas.Num())
	{
		FinishUndoAnimation();
		return;
	}

	// Start the next undo wave group. Like the forward wave propagation, the group is
	// applied under a per-frame CPU budget: whatever doesn't fit continues in Tick.
	PeekNextUndoGroup(CurrentUndoGroupPriority, CurrentUndoGroupDistance);
	bUndoGroupInProgress = true;
	ContinueUndoGroupSlice();
}

void UML_RollBackSubsystem::ContinueUndoGroupSlice()
{
	const bool bGroupFullyDrained = ApplyUndoWaveGroup(CurrentUndoGroupPriority, CurrentUndoGroupDistance, MakeUndoSliceDeadline());

	// Budget exhausted: resume this group next frame (Tick).
	if (!bGroupFullyDrained)
		return;

	bUndoGroupInProgress = false;

	if (PendingUndoTileIndex >= PendingUndoTileDeltas.Num()
		&& PendingUndoSpawnIndex >= PendingUndoSpawnDeltas.Num())
	{
		FinishUndoAnimation();
		return;
	}

	int32 NextPri = 0;
	int32 NextDist = 0;
	PeekNextUndoGroup(NextPri, NextDist);

	const float Delay = (NextPri != CurrentUndoGroupPriority) ? DevSettings->InterWaveDelay : DevSettings->IntraWaveDelay;
	ScheduleNextUndoWave(Delay);
}

bool UML_RollBackSubsystem::ApplyUndoWaveGroup(int32 PriorityIndex, int32 DistanceFromOrigin, double Deadline)
{
	int32 ItemsProcessed = 0;

	// Always process at least one item per slice regardless of budget, so a single
	// very expensive item (or clock jitter) can't stall the wave indefinitely.
	auto BudgetExceeded = [Deadline, &ItemsProcessed]()
	{
		return ItemsProcessed > 0 && FPlatformTime::Seconds() >= Deadline;
	};

	// Deltas are sorted by (Priority, Distance), so the current group's items are
	// contiguous at each read cursor: consume in place, no removal.
	while (PendingUndoSpawnIndex < PendingUndoSpawnDeltas.Num() && !BudgetExceeded())
	{
		const FML_SpawnUndoDelta& SpawnDelta = PendingUndoSpawnDeltas[PendingUndoSpawnIndex];
		if (SpawnDelta.PriorityIndex != PriorityIndex || SpawnDelta.DistanceFromOrigin != DistanceFromOrigin)
			break;

		if (AActor* SpawnedActor = SpawnDelta.SpawnedActor.Get())
		{
			if (IsValid(SpawnedActor))
			{
				SpawnedActor->Destroy();
			}
		}

		++PendingUndoSpawnIndex;
		++ItemsProcessed;
	}

	bUndoInProgress = true;

	while (PendingUndoTileIndex < PendingUndoTileDeltas.Num() && !BudgetExceeded())
	{
		const FML_TileUndoDelta& TileDelta = PendingUndoTileDeltas[PendingUndoTileIndex];
		if (TileDelta.PriorityIndex != PriorityIndex || TileDelta.DistanceFromOrigin != DistanceFromOrigin)
			break;

		AML_Tile* Tile = TileDelta.Tile.Get();
		if (IsValid(Tile) && CachedActiveUndoTileSet)
		{
			const EML_TileType PreviousType = Tile->GetCurrentType();
			const EML_TileType NewType = TileDelta.OldType;

			Tile->UpdateClassAtRuntime_Silent(NewType, CachedActiveUndoTileSet->GetClassFromTileType(NewType));

			if (PreviousType != NewType)
			{
				Tile->OnTileTypeChanged(PreviousType, NewType);
			}

			const bool bShouldHaveCollectible = TileDelta.bOldHasCollectible && !CachedSpawnedCollectibleAxials.Contains(Tile->GetAxialCoord());
			const bool bHasCollectibleNow = Tile->HasCollectible();

			if (!bShouldHaveCollectible && bHasCollectibleNow)
			{
				DestroyCollectibleActorOnTile(Tile);
				Tile->SetHasCollectible(false);
			}
			else
			{
				Tile->SetHasCollectible(bShouldHaveCollectible);
				if (bShouldHaveCollectible)
				{
					RestoreCollectibleActorOnTile(Tile);
				}
			}

			Tile->bConsumedGrass = TileDelta.bOldConsumedGrass;
		}

		++PendingUndoTileIndex;
		++ItemsProcessed;
	}

	bUndoInProgress = false;

	const bool bSpawnGroupRemaining = PendingUndoSpawnIndex < PendingUndoSpawnDeltas.Num()
		&& PendingUndoSpawnDeltas[PendingUndoSpawnIndex].PriorityIndex == PriorityIndex
		&& PendingUndoSpawnDeltas[PendingUndoSpawnIndex].DistanceFromOrigin == DistanceFromOrigin;

	const bool bTileGroupRemaining = PendingUndoTileIndex < PendingUndoTileDeltas.Num()
		&& PendingUndoTileDeltas[PendingUndoTileIndex].PriorityIndex == PriorityIndex
		&& PendingUndoTileDeltas[PendingUndoTileIndex].DistanceFromOrigin == DistanceFromOrigin;

	return !bSpawnGroupRemaining && !bTileGroupRemaining;
}

void UML_RollBackSubsystem::ScheduleNextUndoWave(float Delay)
{
	if (!GetWorld()) return;

	// Timer runs in dilated game time; clamp so real spacing between waves never collapses.
	const float SafeDelay = FMath::Max(Delay, MinRealSecondsPerWave * AppliedTimeDilation);

	GetWorld()->GetTimerManager().SetTimer(
		UndoWaveTimerHandle,
		this,
		&UML_RollBackSubsystem::RunUndoWave,
		SafeDelay,
		false
	);
}

void UML_RollBackSubsystem::FinishUndoAnimation()
{
	bIsUndoAnimating = false;
	bUndoInProgress = false;
	bUndoGroupInProgress = false;

	PendingUndoTileDeltas.Reset();
	PendingUndoSpawnDeltas.Reset();
	PendingUndoTileIndex = 0;
	PendingUndoSpawnIndex = 0;
	ActiveUndoRecord = FML_TurnUndoRecord{};
	CachedSpawnedCollectibleAxials.Reset();
	CachedActiveUndoTileSet = nullptr;

	if (!bIsResetAllAnimating && !bIsUndoUntilPlantAnimating)
	{
		ClearTimeDilation();
		OnUndoAnimating.Broadcast(bIsUndoAnimating);
	}

	if (bIsResetAllAnimating)
	{
		ContinueResetAllIfNeeded();
	}
	else if (bIsUndoUntilPlantAnimating)
	{
		ContinueUndoUntilPlantIfNeeded();
	}
	else if (PlayerController)
	{
		PlayerController->EnableInput(PlayerController);
	}
}

bool UML_RollBackSubsystem::RestoreCollectibleDuringUndoMove(const FIntPoint& Axial)
{
	EnsureInitialized();
	if (!GetWorld() || !PlayerController) return false;

	AML_PlayerCharacter* PC = Cast<AML_PlayerCharacter>(PlayerController->GetPawn());
	if (!IsValid(PC) || !IsValid(PC->CurrentTileOn)) return false;

	AML_BoardSpawner* Board = Cast<AML_BoardSpawner>(PC->CurrentTileOn->GetOwner());
	if (!IsValid(Board)) return false;

	UML_BiomeTileSet* TileSet = Board->GetBiomeTileSet();
	if (!IsValid(TileSet)) return false;

	TSubclassOf<AML_Collectible> CollectibleClass = TileSet->GetCollectibleClass();
	if (!*CollectibleClass) return false;

	const TMap<FIntPoint, AML_Tile*> GridMap = Board->GetGridMap();
	AML_Tile* const* TilePtr = GridMap.Find(Axial);
	if (!TilePtr || !IsValid(*TilePtr)) return false;

	AML_Tile* Tile = *TilePtr;
	if (Tile->HasCollectible()) return false;

	const FVector SpawnLocation = Tile->GetActorLocation();

	AML_Collectible* SpawnedCollectible = GetWorld()->SpawnActorDeferred<AML_Collectible>(
		CollectibleClass,
		FTransform(FRotator::ZeroRotator, SpawnLocation),
		nullptr,
		nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn
	);

	if (SpawnedCollectible)
	{
		SpawnedCollectible->SetOwningTile(*TilePtr);
		SpawnedCollectible->FinishSpawning(FTransform(FRotator::ZeroRotator, SpawnLocation));
	}

	if (!IsValid(SpawnedCollectible)) return false;

	SpawnedCollectible->InitOwningAxial(Axial);
	Tile->CollectibleActor = SpawnedCollectible;
	Tile->SetHasCollectible(true);

	PlayerController->EnergyComponent->AddEnergy(-1);

	if (UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(SpawnedCollectible->GetRootComponent()))
	{
		Prim->SetCollisionEnabled(ECollisionEnabled::NoCollision);

		FTimerHandle Tmp;
		GetWorld()->GetTimerManager().SetTimer(Tmp, [Prim]()
		{
			if (IsValid(Prim))
			{
				Prim->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			}
		}, 0.05f, false);
	}

	return true;
}

TSet<FIntPoint> UML_RollBackSubsystem::GetSpawnedCollectibleAxials(const TArray<FML_SpawnUndoDelta>& SpawnDeltas) const
{
	TSet<FIntPoint> SpawnedCollectibleAxials;
	for (const FML_SpawnUndoDelta& SpawnDelta : SpawnDeltas)
	{
		if (SpawnDelta.bIsCollectible)
		{
			SpawnedCollectibleAxials.Add(SpawnDelta.CollectibleAxial);
		}
	}

	return SpawnedCollectibleAxials;
}

bool UML_RollBackSubsystem::RestoreCollectibleActorOnTile(AML_Tile* Tile)
{
	if (!GetWorld() || !IsValid(Tile)) return false;

	if (AML_Collectible* ExistingCollectible = Tile->CollectibleActor.Get())
	{
		if (IsValid(ExistingCollectible))
		{
			Tile->SetHasCollectible(true);
			return true;
		}
	}

	AML_BoardSpawner* Board = Tile->GetBoardSpawnerFromTile();
	if (!IsValid(Board)) return false;

	UML_BiomeTileSet* TileSet = Board->GetBiomeTileSet();
	if (!IsValid(TileSet)) return false;

	TSubclassOf<AML_Collectible> CollectibleClass = TileSet->GetCollectibleClass();
	if (!*CollectibleClass) return false;

	const FVector SpawnLocation = Tile->GetActorLocation();
	AML_Collectible* SpawnedCollectible = GetWorld()->SpawnActorDeferred<AML_Collectible>(
		CollectibleClass,
		FTransform(FRotator::ZeroRotator, SpawnLocation),
		nullptr,
		nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn
	);

	if (SpawnedCollectible)
	{
		SpawnedCollectible->SetOwningTile(Tile);
		SpawnedCollectible->FinishSpawning(FTransform(FRotator::ZeroRotator, SpawnLocation));
	}

	if (!IsValid(SpawnedCollectible)) return false;

	SpawnedCollectible->InitOwningAxial(Tile->GetAxialCoord());
	Tile->CollectibleActor = SpawnedCollectible;
	Tile->SetHasCollectible(true);

	return true;
}

void UML_RollBackSubsystem::DestroyCollectibleActorOnTile(AML_Tile* Tile)
{
	if (!IsValid(Tile)) return;

	if (AML_Collectible* Collectible = Tile->CollectibleActor.Get())
	{
		if (IsValid(Collectible))
		{
			Collectible->DestroyCollectible();
		}
	}
}

void UML_RollBackSubsystem::NotifyMoveCompleted(
	const FIntPoint& StartAxial,
	const FIntPoint& EndAxial,
	const TArray<FIntPoint>& AxialPath,
	const FVector& StartWorld,
	const FVector& EndWorld,
	const TArray<FIntPoint>& PickedCollectibleAxials)
{
	EnsureInitialized();

	if (!PlayerController) return;
	if (bIsUndoAnimating || bIsResetAllAnimating) return;

	const UML_WavePropagationSubsystem* WaveSubsystem = GetWorld() ? GetWorld()->GetSubsystem<UML_WavePropagationSubsystem>() : nullptr;
	if (WaveSubsystem && WaveSubsystem->IsResolvingTiles()) return;

	if (AxialPath.Num() < 2) return;
	if (StartAxial == EndAxial) return;

	TArray<FML_ActionUndoRecord>* Stack = GetCurrentBoardStack();
	if (!Stack) return;

	FML_ActionUndoRecord Action;
	Action.Type = EML_UndoActionType::Move;
	Action.Move.StartAxial = StartAxial;
	Action.Move.EndAxial = EndAxial;
	Action.Move.AxialPath = AxialPath;
	Action.Move.StartWorld = StartWorld;
	Action.Move.EndWorld = EndWorld;
	Action.Move.PickedCollectibleAxials = PickedCollectibleAxials;

	Stack->Add(Action);
}

void UML_RollBackSubsystem::ApplyUndoTimeDilation(const TArray<FML_ActionUndoRecord>& Stack)
{
	if (!GetWorld() || bUndoTimeDilationApplied) return;

	const UML_MycelandDeveloperSettings* Settings = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();
	const float MinRollBackSpeed = Settings ? FMath::Max(Settings->RollBackMinSpeed, 0.01f) : 0.01f;
	const float MaxRollBackSpeed = Settings ? FMath::Max(Settings->RollBackMaxSpeed, MinRollBackSpeed) : MinRollBackSpeed;
	if (!Settings || !Settings->bUseDynamicRollBackSpeed)
	{
		const float FixedSpeed = Settings ? Settings->UndoSpeed : 1.0f;
		AppliedTimeDilation = FMath::Clamp(FixedSpeed, MinRollBackSpeed, MaxRollBackSpeed);
		UGameplayStatics::SetGlobalTimeDilation(GetWorld(), AppliedTimeDilation);
		bUndoTimeDilationApplied = true;
		return;
	}

	const float EstimatedGameDuration = EstimateUndoGameDuration(Stack);
	const float TargetDuration = Settings->ResetTargetDuration;
	const float UndoTimeDilation = (EstimatedGameDuration > KINDA_SMALL_NUMBER && TargetDuration > KINDA_SMALL_NUMBER)
		? EstimatedGameDuration / TargetDuration
		: Settings->UndoSpeed;

	AppliedTimeDilation = FMath::Clamp(UndoTimeDilation, MinRollBackSpeed, MaxRollBackSpeed);
	UGameplayStatics::SetGlobalTimeDilation(GetWorld(), AppliedTimeDilation);
	bUndoTimeDilationApplied = true;
}

void UML_RollBackSubsystem::ApplyResetTimeDilation(const TArray<FML_ActionUndoRecord>& Stack)
{
	if (!GetWorld() || bUndoTimeDilationApplied) return;

	const UML_MycelandDeveloperSettings* Settings = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();
	const float MinRollBackSpeed = Settings ? FMath::Max(Settings->RollBackMinSpeed, 0.01f) : 0.01f;
	const float MaxRollBackSpeed = Settings ? FMath::Max(Settings->RollBackMaxSpeed, MinRollBackSpeed) : MinRollBackSpeed;
	if (!Settings || !Settings->bUseDynamicRollBackSpeed)
	{
		const float FixedSpeed = Settings ? Settings->ResetSpeed : 1.0f;
		AppliedTimeDilation = FMath::Clamp(FixedSpeed, MinRollBackSpeed, MaxRollBackSpeed);
		UGameplayStatics::SetGlobalTimeDilation(GetWorld(), AppliedTimeDilation);
		bUndoTimeDilationApplied = true;
		return;
	}

	const float EstimatedGameDuration = EstimateResetGameDuration(Stack);
	const float TargetDuration = Settings->ResetTargetDuration;
	const float ResetTimeDilation = (EstimatedGameDuration > KINDA_SMALL_NUMBER && TargetDuration > KINDA_SMALL_NUMBER)
		? EstimatedGameDuration / TargetDuration
		: Settings->ResetSpeed;

	AppliedTimeDilation = FMath::Clamp(ResetTimeDilation, MinRollBackSpeed, MaxRollBackSpeed);
	UGameplayStatics::SetGlobalTimeDilation(GetWorld(), AppliedTimeDilation);
	bUndoTimeDilationApplied = true;
}

float UML_RollBackSubsystem::EstimateUndoGameDuration(const TArray<FML_ActionUndoRecord>& Stack) const
{
	if (Stack.Num() == 0)
	{
		return 0.0f;
	}

	if (!bIsUndoUntilPlantAnimating)
	{
		return EstimateActionResetGameDuration(Stack.Last());
	}

	float Duration = 0.0f;
	for (int32 Index = Stack.Num() - 1; Index >= 0; --Index)
	{
		const FML_ActionUndoRecord& Action = Stack[Index];
		Duration += EstimateActionResetGameDuration(Action);

		if (Action.Type == EML_UndoActionType::PlantWaves)
		{
			break;
		}
	}

	return Duration;
}

float UML_RollBackSubsystem::EstimateResetGameDuration(const TArray<FML_ActionUndoRecord>& Stack) const
{
	float Duration = 0.0f;

	for (const FML_ActionUndoRecord& Action : Stack)
	{
		Duration += EstimateActionResetGameDuration(Action);
	}

	return Duration;
}

float UML_RollBackSubsystem::EstimateActionResetGameDuration(const FML_ActionUndoRecord& Action) const
{
	switch (Action.Type)
	{
	case EML_UndoActionType::Move:
		return EstimateMoveResetGameDuration(Action.Move);
	case EML_UndoActionType::PlantWaves:
		return EstimatePlantResetGameDuration(Action.Turn);
	default:
		return 0.0f;
	}
}

float UML_RollBackSubsystem::EstimateMoveResetGameDuration(const FML_MoveUndoRecord& Move) const
{
	if (!PlayerCharacter || !PlayerCharacter->CurrentTileOn || Move.AxialPath.Num() < 2)
	{
		return 0.0f;
	}

	const AML_BoardSpawner* Board = PlayerCharacter->CurrentTileOn->GetBoardSpawnerFromTile();
	if (!IsValid(Board))
	{
		return 0.0f;
	}

	const TMap<FIntPoint, AML_Tile*> GridMap = Board->GetGridMap();
	float PathDistance = 0.0f;

	for (int32 Index = 1; Index < Move.AxialPath.Num(); ++Index)
	{
		AML_Tile* const* PreviousTile = GridMap.Find(Move.AxialPath[Index - 1]);
		AML_Tile* const* CurrentTile = GridMap.Find(Move.AxialPath[Index]);
		if (PreviousTile && CurrentTile && IsValid(*PreviousTile) && IsValid(*CurrentTile))
		{
			PathDistance += FVector::Dist2D((*PreviousTile)->GetActorLocation(), (*CurrentTile)->GetActorLocation());
		}
	}

	if (PathDistance <= KINDA_SMALL_NUMBER)
	{
		PathDistance = FVector::Dist2D(Move.StartWorld, Move.EndWorld);
	}

	const UCharacterMovementComponent* MovementComponent = PlayerCharacter->GetCharacterMovement();
	const float MovementSpeed = MovementComponent ? MovementComponent->MaxWalkSpeed : 0.0f;
	const float MoveSpeedScale = PlayerController ? PlayerController->GetMoveSpeedScale() : 1.0f;
	const float EffectiveSpeed = MovementSpeed * FMath::Max(MoveSpeedScale, 0.01f);

	return EffectiveSpeed > KINDA_SMALL_NUMBER ? PathDistance / EffectiveSpeed : 0.0f;
}

float UML_RollBackSubsystem::EstimatePlantResetGameDuration(const FML_TurnUndoRecord& Turn) const
{
	if (!DevSettings)
	{
		return 0.0f;
	}

	// TSet dedup instead of TArray::Contains (was O(n^2) per turn, now O(n)).
	TSet<FIntPoint> UniqueGroups;
	UniqueGroups.Reserve(Turn.TileDeltas.Num() + Turn.SpawnDeltas.Num());

	for (const FML_TileUndoDelta& Delta : Turn.TileDeltas)
	{
		UniqueGroups.Add(FIntPoint(Delta.PriorityIndex, Delta.DistanceFromOrigin));
	}

	for (const FML_SpawnUndoDelta& Delta : Turn.SpawnDeltas)
	{
		UniqueGroups.Add(FIntPoint(Delta.PriorityIndex, Delta.DistanceFromOrigin));
	}

	TArray<FIntPoint> OrderedGroups = UniqueGroups.Array();
	OrderedGroups.Sort([](const FIntPoint& A, const FIntPoint& B)
	{
		if (A.X != B.X) return A.X > B.X;
		return A.Y > B.Y;
	});

	float Duration = 0.0f;
	for (int32 Index = 1; Index < OrderedGroups.Num(); ++Index)
	{
		Duration += OrderedGroups[Index].X != OrderedGroups[Index - 1].X
			? DevSettings->InterWaveDelay
			: DevSettings->IntraWaveDelay;
	}

	return Duration;
}

void UML_RollBackSubsystem::ClearTimeDilation()
{
	if (!GetWorld() || !bUndoTimeDilationApplied) return;

	UGameplayStatics::SetGlobalTimeDilation(GetWorld(), 1.0f);
	AppliedTimeDilation = 1.0f;
	bUndoTimeDilationApplied = false;
}

bool UML_RollBackSubsystem::ResetAllActions_Animated()
{
	EnsureInitialized();
	if (!PlayerController || !DevSettings) return false;
	if (bIsUndoAnimating || bIsResetAllAnimating) return false;
	if (IsCurrentBoardSolved()) return false; // no reset once the puzzle is won

	UML_WavePropagationSubsystem* WaveSubsystem = GetWorld() ? GetWorld()->GetSubsystem<UML_WavePropagationSubsystem>() : nullptr;
	if (!WaveSubsystem || WaveSubsystem->IsResolvingTiles()) return false;

	AML_PlayerCharacter* PC = Cast<AML_PlayerCharacter>(PlayerController->GetPawn());
	if (!PC || !PC->CurrentTileOn) return false;

	TArray<FML_ActionUndoRecord>* Stack = GetCurrentBoardStack();
	if (!Stack || Stack->Num() == 0) return false;

	WaveSubsystem->CancelAllWaveTimers();
	PlayerController->DisableInput(PlayerController);
	ApplyResetTimeDilation(*Stack);

	bIsResetAllAnimating = true;
	OnResetAnimating.Broadcast(bIsResetAllAnimating);

	const bool bStarted = UndoSingleAction_Animated();
	if (!bStarted)
	{
		bIsResetAllAnimating = false;
		ClearTimeDilation();
		OnResetAnimating.Broadcast(bIsResetAllAnimating);
		PlayerController->EnableInput(PlayerController);
		return false;
	}

	if (UML_SoundSubsystem* SoundSubsystem = UML_SoundSubsystem::Get(this))
	{
		SoundSubsystem->StartSound2DByPath(MLFMODEvents::TimelineReset);
	}

	return true;
}

void UML_RollBackSubsystem::StartNextResetUndoStep()
{
	if (!bIsResetAllAnimating || bIsUndoAnimating) return;

	UML_WavePropagationSubsystem* WaveSubsystem = GetWorld() ? GetWorld()->GetSubsystem<UML_WavePropagationSubsystem>() : nullptr;
	if (WaveSubsystem && WaveSubsystem->IsResolvingTiles()) return;

	TArray<FML_ActionUndoRecord>* Stack = GetCurrentBoardStack();
	if (!Stack || Stack->Num() == 0)
	{
		bIsResetAllAnimating = false;
		ClearTimeDilation();
		OnResetAnimating.Broadcast(bIsResetAllAnimating);

		if (PlayerController)
		{
			PlayerController->EnableInput(PlayerController);
		}
		return;
	}

	const bool bStarted = UndoSingleAction_Animated();
	if (!bStarted)
	{
		bIsResetAllAnimating = false;
		ClearTimeDilation();
		OnResetAnimating.Broadcast(bIsResetAllAnimating);

		if (PlayerController)
		{
			PlayerController->EnableInput(PlayerController);
		}
	}
}

void UML_RollBackSubsystem::ContinueResetAllIfNeeded()
{
	if (!bIsResetAllAnimating)
	{
		if (PlayerController)
		{
			PlayerController->EnableInput(PlayerController);
		}
		return;
	}

	TArray<FML_ActionUndoRecord>* Stack = GetCurrentBoardStack();
	if (!Stack || Stack->Num() == 0 || !GetWorld())
	{
		bIsResetAllAnimating = false;
		ClearTimeDilation();
		OnResetAnimating.Broadcast(bIsResetAllAnimating);

		if (PlayerController)
		{
			PlayerController->EnableInput(PlayerController);
		}
		return;
	}

	GetWorld()->GetTimerManager().SetTimerForNextTick(this, &UML_RollBackSubsystem::StartNextResetUndoStep);
}

bool UML_RollBackSubsystem::UndoLastAction_Animated()
{
	EnsureInitialized();
	if (!PlayerController || !DevSettings) return false;
	if (IsCurrentBoardSolved()) return false; // no undo once the puzzle is won

	bool bStarted = false;
	if (DevSettings->bUndoUntilPlant && !bIsResetAllAnimating)
	{
		bStarted = UndoUntilPlant_Animated();
	}
	else
	{
		bStarted = UndoSingleAction_Animated();
	}

	if (bStarted)
	{
		if (UML_SoundSubsystem* SoundSubsystem = UML_SoundSubsystem::Get(this))
		{
			SoundSubsystem->StartSound2DByPath(MLFMODEvents::TimelineUndo);
		}
	}

	return bStarted;
}

bool UML_RollBackSubsystem::UndoUntilPlant_Animated()
{
	if (!PlayerController || !DevSettings) return false;
	if (bIsUndoAnimating || bIsUndoUntilPlantAnimating) return false;
	if (!CanUndo()) return false;

	bIsUndoUntilPlantAnimating = true;
	bUndoUntilPlantReachedPlant = false;
	OnUndoAnimating.Broadcast(true);

	const bool bStarted = UndoSingleAction_Animated();
	if (!bStarted)
	{
		bIsUndoUntilPlantAnimating = false;
		bUndoUntilPlantReachedPlant = false;
		bIsUndoAnimating = false;
		ClearTimeDilation();
		OnUndoAnimating.Broadcast(bIsUndoAnimating);
		PlayerController->EnableInput(PlayerController);
		return false;
	}

	return true;
}

void UML_RollBackSubsystem::StartNextUndoUntilPlantStep()
{
	if (!bIsUndoUntilPlantAnimating || bIsUndoAnimating) return;

	UML_WavePropagationSubsystem* WaveSubsystem = GetWorld() ? GetWorld()->GetSubsystem<UML_WavePropagationSubsystem>() : nullptr;
	if (WaveSubsystem && WaveSubsystem->IsResolvingTiles())
	{
		if (GetWorld())
		{
			GetWorld()->GetTimerManager().SetTimerForNextTick(this, &UML_RollBackSubsystem::StartNextUndoUntilPlantStep);
		}
		return;
	}

	const bool bStarted = UndoSingleAction_Animated();
	if (!bStarted)
	{
		bIsUndoUntilPlantAnimating = false;
		bUndoUntilPlantReachedPlant = false;
		ClearTimeDilation();
		OnUndoAnimating.Broadcast(false);

		if (PlayerController)
		{
			PlayerController->EnableInput(PlayerController);
		}
	}
}

void UML_RollBackSubsystem::ContinueUndoUntilPlantIfNeeded()
{
	if (!bIsUndoUntilPlantAnimating)
	{
		if (PlayerController)
		{
			PlayerController->EnableInput(PlayerController);
		}
		return;
	}

	TArray<FML_ActionUndoRecord>* Stack = GetCurrentBoardStack();
	if (bUndoUntilPlantReachedPlant || !Stack || Stack->Num() == 0 || !GetWorld())
	{
		bIsUndoUntilPlantAnimating = false;
		bUndoUntilPlantReachedPlant = false;
		ClearTimeDilation();
		OnUndoAnimating.Broadcast(false);

		if (PlayerController)
		{
			PlayerController->EnableInput(PlayerController);
		}
		return;
	}

	GetWorld()->GetTimerManager().SetTimerForNextTick(this, &UML_RollBackSubsystem::StartNextUndoUntilPlantStep);
}

bool UML_RollBackSubsystem::UndoSingleAction_Animated()
{
	EnsureInitialized();
	if (!PlayerController || !DevSettings) return false;
	if (bIsUndoAnimating) return false;

	UML_WavePropagationSubsystem* WaveSubsystem = GetWorld() ? GetWorld()->GetSubsystem<UML_WavePropagationSubsystem>() : nullptr;
	if (!WaveSubsystem || WaveSubsystem->IsResolvingTiles()) return false;

	AML_PlayerCharacter* PC = Cast<AML_PlayerCharacter>(PlayerController->GetPawn());
	if (!PC || !PC->CurrentTileOn) return false;

	TArray<FML_ActionUndoRecord>* Stack = GetCurrentBoardStack();
	if (!Stack || Stack->Num() == 0) return false;

	WaveSubsystem->CancelAllWaveTimers();
	PlayerController->DisableInput(PlayerController);
	ApplyUndoTimeDilation(*Stack);

	const FML_ActionUndoRecord Action = Stack->Pop();
	if (bIsUndoUntilPlantAnimating && Action.Type == EML_UndoActionType::PlantWaves)
	{
		bUndoUntilPlantReachedPlant = true;
	}

	if (Action.Type == EML_UndoActionType::Move)
	{
		bIsUndoAnimating = true;
		if (!bIsResetAllAnimating && !bIsUndoUntilPlantAnimating)
		{
			OnUndoAnimating.Broadcast(bIsUndoAnimating);
		}

		TArray<FIntPoint> ReversePath = Action.Move.AxialPath;
		Algo::Reverse(ReversePath);

		PlayerController->StartMoveAlongAxialPathForUndo(ReversePath, Action.Move.PickedCollectibleAxials);
		return true;
	}

	if (Action.Type == EML_UndoActionType::PlantWaves)
	{
		bIsUndoAnimating = true;
		if (!bIsResetAllAnimating && !bIsUndoUntilPlantAnimating)
		{
			OnUndoAnimating.Broadcast(bIsUndoAnimating);
		}

		ActiveUndoRecord = Action.Turn;
		PlayerController->EnergyComponent->SetCurrentEnergy(ActiveUndoRecord.EnergyBefore);

		CachedSpawnedCollectibleAxials = GetSpawnedCollectibleAxials(ActiveUndoRecord.SpawnDeltas);
		CachedActiveUndoTileSet = nullptr;
		if (AML_BoardSpawner* Board = PC->CurrentTileOn->GetBoardSpawnerFromTile())
		{
			CachedActiveUndoTileSet = Board->GetBiomeTileSet();
		}

		PendingUndoTileDeltas = ActiveUndoRecord.TileDeltas;
		PendingUndoSpawnDeltas = ActiveUndoRecord.SpawnDeltas;
		PendingUndoTileIndex = 0;
		PendingUndoSpawnIndex = 0;

		PendingUndoTileDeltas.Sort([](const FML_TileUndoDelta& A, const FML_TileUndoDelta& B)
		{
			if (A.PriorityIndex != B.PriorityIndex) return A.PriorityIndex > B.PriorityIndex;
			if (A.DistanceFromOrigin != B.DistanceFromOrigin) return A.DistanceFromOrigin > B.DistanceFromOrigin;
			return A.Sequence > B.Sequence;
		});

		PendingUndoSpawnDeltas.Sort([](const FML_SpawnUndoDelta& A, const FML_SpawnUndoDelta& B)
		{
			if (A.PriorityIndex != B.PriorityIndex) return A.PriorityIndex > B.PriorityIndex;
			if (A.DistanceFromOrigin != B.DistanceFromOrigin) return A.DistanceFromOrigin > B.DistanceFromOrigin;
			return A.Sequence > B.Sequence;
		});

		RunUndoWave();
		return true;
	}

	if (bIsResetAllAnimating)
	{
		ContinueResetAllIfNeeded();
	}
	else
	{
		ClearTimeDilation();
		if (PlayerController)
		{
			PlayerController->EnableInput(PlayerController);
		}
	}

	return false;
}

void UML_RollBackSubsystem::ResetRuntimeState()
{
	// The animating flags are cleared silently here (no OnUndoAnimating / OnResetAnimating broadcast), so
	// the board token taken when the animation started would never be released. Drop it explicitly.
	if (UML_BoardActionSubsystem* BoardAction = UML_BoardActionSubsystem::Get(this))
		BoardAction->EndBoardAction(this);

	bIsResetAllAnimating = false;
	bIsUndoUntilPlantAnimating = false;
	bUndoUntilPlantReachedPlant = false;
	bIsUndoAnimating = false;
	bUndoInProgress = false;
	bUndoGroupInProgress = false;

	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(UndoWaveTimerHandle);
	}

	PendingUndoTileDeltas.Reset();
	PendingUndoSpawnDeltas.Reset();
	PendingUndoTileIndex = 0;
	PendingUndoSpawnIndex = 0;
	ActiveUndoRecord = FML_TurnUndoRecord{};
	CachedSpawnedCollectibleAxials.Reset();
	CachedActiveUndoTileSet = nullptr;
	ClearTimeDilation();
}

void UML_RollBackSubsystem::ResetAllActions_ExcludingMoves_Instant(AML_BoardSpawner* Board)
{
	EnsureInitialized();
	if (!PlayerController || !IsValid(Board)) return;

	UML_WavePropagationSubsystem* WaveSubsystem = GetWorld() ? GetWorld()->GetSubsystem<UML_WavePropagationSubsystem>() : nullptr;
	if (!WaveSubsystem) return;

	if (WaveSubsystem->IsResolvingTiles() && bHasActiveTurnRecord)
	{
		CommitTurnRecord();
	}
	else
	{
		DiscardTurnRecord();
	}

	TArray<FML_ActionUndoRecord>* Stack = BoardUndoStacks.Find(Board);
	if (!Stack)
	{
		WaveSubsystem->AbortPropagationRuntime();
		ResetRuntimeState();
		if (PlayerController)
		{
			PlayerController->EnableInput(PlayerController);
		}
		return;
	}

	Stack->RemoveAll([](const FML_ActionUndoRecord& Action)
	{
		return Action.Type == EML_UndoActionType::Move;
	});

	for (int32 i = 0; i < Stack->Num(); ++i)
	{
		if ((*Stack)[i].Type == EML_UndoActionType::PlantWaves)
		{
			PlayerController->EnergyComponent->SetCurrentEnergy((*Stack)[i].Turn.EnergyBefore);
			break;
		}
	}

	for (int32 i = Stack->Num() - 1; i >= 0; --i)
	{
		const FML_ActionUndoRecord& Action = (*Stack)[i];
		if (Action.Type != EML_UndoActionType::PlantWaves) continue;

		const FML_TurnUndoRecord& Turn = Action.Turn;
		const TSet<FIntPoint> SpawnedCollectibleAxials = GetSpawnedCollectibleAxials(Turn.SpawnDeltas);

		for (const FML_SpawnUndoDelta& SpawnDelta : Turn.SpawnDeltas)
		{
			if (AActor* SpawnedActor = SpawnDelta.SpawnedActor.Get())
			{
				if (IsValid(SpawnedActor))
				{
					SpawnedActor->Destroy();
				}
			}
		}

		TSet<AML_Tile*> AlreadyReverted;
		for (const FML_TileUndoDelta& TileDelta : Turn.TileDeltas)
		{
			AML_Tile* Tile = TileDelta.Tile.Get();
			if (!IsValid(Tile) || AlreadyReverted.Contains(Tile)) continue;

			AlreadyReverted.Add(Tile);

			const UML_BiomeTileSet* TileSet = Tile->GetBoardSpawnerFromTile()->GetBiomeTileSet();
			if (!TileSet) continue;

			const EML_TileType PreviousType = Tile->GetCurrentType();
			const EML_TileType NewType = TileDelta.OldType;

			Tile->UpdateClassAtRuntime_Silent(NewType, TileSet->GetClassFromTileType(NewType));

			if (PreviousType != NewType)
			{
				Tile->OnTileTypeChanged(PreviousType, NewType);
			}

			const bool bShouldHaveCollectible = TileDelta.bOldHasCollectible && !SpawnedCollectibleAxials.Contains(Tile->GetAxialCoord());
			const bool bHasCollectibleNow = Tile->HasCollectible();

			if (!bShouldHaveCollectible && bHasCollectibleNow)
			{
				DestroyCollectibleActorOnTile(Tile);
				Tile->SetHasCollectible(false);
			}
			else
			{
				Tile->SetHasCollectible(bShouldHaveCollectible);
				if (bShouldHaveCollectible)
				{
					RestoreCollectibleActorOnTile(Tile);
				}
			}

			Tile->bConsumedGrass = TileDelta.bOldConsumedGrass;
		}
	}

	BoardUndoStacks.Remove(Board);
	WaveSubsystem->AbortPropagationRuntime();
	ResetRuntimeState();

	if (PlayerController)
	{
		PlayerController->EnableInput(PlayerController);
	}
}