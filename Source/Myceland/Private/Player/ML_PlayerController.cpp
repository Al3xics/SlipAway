// Copyright Myceland Team, All Rights Reserved.

#include "Player/ML_PlayerController.h"

#include "Audio/ML_FMODEvents.h"
#include "EngineUtils.h"
#include "Actors/ML_CameraRail.h"
#include "Component/ML_EnergyComponent.h"
#include "Subsystem/ML_NarrativeSubsystem.h"
#include "Component/ML_HoverPreviewComponent.h"
#include "Component/ML_MoveRecordingComponent.h"
#include "Core/ML_TileTypeTraits.h"
#include "Player/ML_HexPathfinder.h"
#include "Component/ML_BoardTransitionComponent.h"
#include "Camera/CameraActor.h"
#include "Components/SplineComponent.h"
#include "Developer Settings/ML_MycelandDeveloperSettings.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedInputComponent.h"
#include "InputAction.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Framework/Application/SlateApplication.h"
#include "Input/ML_InputDeviceManager.h"
#include "Player/ML_PlayerCharacter.h"
#include "Subsystem/ML_RollBackSubsystem.h"
#include "Subsystem/ML_BoardActionSubsystem.h"
#include "Subsystem/ML_CheatSubsystem.h"
#include "Subsystem/ML_WavePropagationSubsystem.h"
#include "Subsystem/ML_SoundSubsystem.h"
#include "Tiles/ML_Tile.h"
#include "UI/ML_WidgetBase.h"

class UML_WavePropagationSubsystem;

// ==================== Constructor ====================

AML_PlayerController::AML_PlayerController()
{
	EnergyComponent = CreateDefaultSubobject<UML_EnergyComponent>(TEXT("EnergyComponent"));
	HoverPreviewComponent = CreateDefaultSubobject<UML_HoverPreviewComponent>(TEXT("HoverPreviewComponent"));
	MoveRecordingComponent = CreateDefaultSubobject<UML_MoveRecordingComponent>(TEXT("MoveRecordingComponent"));
	TransitionComponent = CreateDefaultSubobject<UML_BoardTransitionComponent>(TEXT("TransitionComponent"));
	NavigationBridgeComponent = CreateDefaultSubobject<UML_NavigationBridgeComponent>(TEXT("NavigationBridgeComponent"));

	MouseKeyboardHandler = CreateDefaultSubobject<UML_MouseKeyboardInputHandler>(TEXT("MouseKeyboardHandler"));
	GamepadHandler = CreateDefaultSubobject<UML_GamepadInputHandler>(TEXT("GamepadHandler"));
	InputDeviceManager = CreateDefaultSubobject<UML_InputDeviceManager>(TEXT("InputDeviceManager"));
}

// ==================== Movement - Path Tick & Callbacks ====================

void AML_PlayerController::TickMoveAlongPath(float DeltaTime)
{
	// Early exit if not moving
	if (!bIsMoving)
		return;

	// Handle nav mesh movement (FreeMovement, EnteringBoard, and the ExitingBoard walk-out off the board)
	const EML_PlayerMovementMode NavMode = TransitionComponent->GetMovementMode();
	if (NavMode == EML_PlayerMovementMode::FreeMovement ||
	    NavMode == EML_PlayerMovementMode::EnteringBoard ||
	    NavMode == EML_PlayerMovementMode::ExitingBoard)
	{
		if (NavigationBridgeComponent && NavigationBridgeComponent->TickNavMeshMovement(DeltaTime))
		{
			SetIsMoving(false);

			// Walk-out finished while still ExitingBoard: the player never crossed off the board footprint
			// (e.g. the exit plane sits on a border tile), so HandleBoardStateChanged never flipped the mode.
			// Force FreeMovement here so the player is never stuck unable to move.
			if (TransitionComponent->GetMovementMode() == EML_PlayerMovementMode::ExitingBoard)
			{
				SetMovementMode(EML_PlayerMovementMode::FreeMovement);
			}
			// Never trigger board entry while a cinematic is running — the narrative subsystem
			// owns player movement during that window.
			else if (TransitionComponent->IsPendingBoardEntry() && !bInCinematicMode)
			{
				OnPathFinished();
			}
		}
		return;
	}

	// Tile-by-tile movement (InsideBoard and ExitingBoard modes)
	if (CurrentPathWorld.Num() == 0 || CurrentPathIndex >= CurrentPathWorld.Num()) return;

	ensureMsgf(MycelandCharacter, TEXT("Player Character is not set!"));
	if (!IsValid(MycelandCharacter)) return;

	const FVector Loc = MycelandCharacter->GetActorLocation();

	// ---------- Corner cut ----------
	if (CornerCutStrength > 0.f && CurrentPathIndex + 1 < CurrentPathWorld.Num())
	{
		const FVector A = FVector(CurrentPathWorld[CurrentPathIndex].X, CurrentPathWorld[CurrentPathIndex].Y, 0.f);
		const FVector B = FVector(CurrentPathWorld[CurrentPathIndex + 1].X, CurrentPathWorld[CurrentPathIndex + 1].Y,
		                          0.f);
		const FVector P = FVector(Loc.X, Loc.Y, 0.f);
		const FVector AB = B - A;
		const float AB2 = AB.SizeSquared();

		if (AB2 > KINDA_SMALL_NUMBER)
		{
			const float T = FMath::Clamp(FVector::DotProduct(P - A, AB) / AB2, 0.f, 1.f);
			if (FVector::Dist(P, A + T * AB) <= BaseCornerCutDistance * CornerCutStrength)
				CurrentPathIndex++;
		}
	}

	// ---------- Move toward current waypoint ----------
	FVector To = CurrentPathWorld[CurrentPathIndex] - Loc;
	To.Z = 0.f;
	const float Dist = To.Size();

	if (Dist <= AcceptanceRadius)
	{
		const int32 ReachedIndex = CurrentPathIndex;
		CurrentPathIndex++;

		// During undo-move playback, restore collectibles *behind the player*.
		// We restore when the player reaches a tile, meaning they just left the previous one.
		{
			UML_RollBackSubsystem* RollBackSubsystem = GetWorld()->GetSubsystem<UML_RollBackSubsystem>();
			MoveRecordingComponent->TickUndoRestore(ReachedIndex, RollBackSubsystem);
		}

		if (CurrentPathIndex >= CurrentPathWorld.Num())
		{
			UML_RollBackSubsystem* RollBackSubsystem = GetWorld()->GetSubsystem<UML_RollBackSubsystem>();
			if (!MoveRecordingComponent->CommitMoveRecord(MycelandCharacter, RollBackSubsystem))
				return;

			// Snap to exact tile center and kill momentum so the character
			// doesn't slide past due to CMC deceleration.
			const FVector DestCenter = CurrentPathWorld[ReachedIndex];
			const FVector SnappedLoc = FVector(DestCenter.X, DestCenter.Y, MycelandCharacter->GetActorLocation().Z);
			MycelandCharacter->SetActorLocation(SnappedLoc, false, nullptr, ETeleportType::TeleportPhysics);
			if (UCharacterMovementComponent* MC = MycelandCharacter->GetCharacterMovement())
				MC->StopMovementImmediately();

			CurrentPathWorld.Reset();
			CurrentPathIndex = 0;

			// Stop the path tick WITHOUT broadcasting the "stopped" state yet: OnPathFinished settles the
			// board action state to Idle (or starts a new move). We defer the notification so it reflects
			// the FINAL settled state — otherwise consumers would see BoardActionState still == Moving at
			// the moment of the broadcast (the source of the earlier auto-select / highlight timing traps).
			bIsMoving = false;
			OnPathFinished();

			// If OnPathFinished didn't start a new move, notify the stop now that the state has settled.
			if (!bIsMoving)
				TransitionComponent->NotifyIsMoving(false);

			return;
		}
		return;
	}

	MycelandCharacter->AddMovementInput(To / Dist, MoveSpeedScale);
}

void AML_PlayerController::OnPathFinished()
{
	FBoardTransitionCommand Cmd = TransitionComponent->HandlePathFinished(MycelandCharacter);

	if (Cmd.bStartNavMesh)
		StartNavMeshMovement(Cmd.NavMeshTarget);
	else if (Cmd.bStartBoardPath)
		StartMoveAlongPath(Cmd.BoardPath, Cmd.BoardGridMap);
}

void AML_PlayerController::SetIsMoving(bool bNewIsMoving)
{
	bIsMoving = bNewIsMoving;
	TransitionComponent->NotifyIsMoving(bNewIsMoving);
}

// ==================== Movement - Tile Movement ====================

bool AML_PlayerController::Move(AML_Tile* TargetTile, int32 StopBeforeTarget)
{
	// Second belt: the board lock already disables the input component, but Move/Plant are also
	// BlueprintCallable, so an unlucky BP or UMG call must not slip a turn in while one is resolving.
	if (const UML_BoardActionSubsystem* BoardAction = UML_BoardActionSubsystem::Get(this))
	{
		if (BoardAction->IsBoardBusy()) return false;
	}

	if (!IsValid(MycelandCharacter) || !IsValid(MycelandCharacter->CurrentTileOn)) return false;
	if (!IsValid(TargetTile)) return false;

	AML_BoardSpawner* Board = MycelandCharacter->CurrentTileOn->GetBoardSpawnerFromTile();
	if (!IsValid(Board) || TargetTile->GetOwner() != Board) return false;
	if (TransitionComponent->GetBoardActionState() == EML_PlayerBoardActionState::TurningToPlant) return false;

	const TMap<FIntPoint, AML_Tile*> GridMap = Board->GetGridMap();
	const FIntPoint StartAxial = MycelandCharacter->CurrentTileOn->GetAxialCoord();
	const FIntPoint GoalAxial = TargetTile->GetAxialCoord();

	if (!GridMap.Contains(StartAxial) || !GridMap.Contains(GoalAxial)) return false;
	if (!UML_HexPathfinder::IsTileWalkable(GridMap[StartAxial]) ||
		!UML_HexPathfinder::IsTileWalkable(GridMap[GoalAxial])) return false;

	TArray<FIntPoint> AxialPath;
	if (!UML_HexPathfinder::BuildPath_AxialBFS(StartAxial, GoalAxial, GridMap, AxialPath)) return false;

	if (StopBeforeTarget > 0)
	{
		if (AxialPath.Num() <= StopBeforeTarget) return false;
		AxialPath.SetNum(AxialPath.Num() - StopBeforeTarget);
	}

	return StartRecordedBoardMove(AxialPath, GridMap);
}

bool AML_PlayerController::Plant(AML_Tile* TargetTile)
{
	// Second belt: the board lock already disables the input component, but Move/Plant are also
	// BlueprintCallable, so an unlucky BP or UMG call must not slip a turn in while one is resolving.
	if (const UML_BoardActionSubsystem* BoardAction = UML_BoardActionSubsystem::Get(this))
	{
		if (BoardAction->IsBoardBusy()) return false;
	}

	if (TransitionComponent->GetMovementMode() != EML_PlayerMovementMode::InsideBoard) return false;
	if (TransitionComponent->GetBoardActionState() == EML_PlayerBoardActionState::TurningToPlant) return false;
	if (!IsValid(MycelandCharacter) || !IsValid(MycelandCharacter->CurrentTileOn)) return false;
	if (EnergyComponent->GetCurrentEnergy() <= 0) return RejectPlantWithFeedback();
	if (!IsValid(TargetTile)) return RejectPlantWithFeedback();

	AML_BoardSpawner* Board = MycelandCharacter->CurrentTileOn->GetBoardSpawnerFromTile();
	if (!IsValid(Board) || TargetTile->GetOwner() != Board) return RejectPlantWithFeedback();
	if (!TargetTile->IsInteractable()) return RejectPlantWithFeedback();
	if (!UML_TileTypeTraits::CanPlayerPlant(TargetTile->GetCurrentType())) return RejectPlantWithFeedback();

	const TMap<FIntPoint, AML_Tile*> GridMap = Board->GetGridMap();
	FIntPoint StartAxial = MycelandCharacter->CurrentTileOn->GetAxialCoord();
	if (MoveRecordingComponent->IsMoveInProgress() && !MoveRecordingComponent->IsUndoMovePlayback())
	{
		const TArray<FIntPoint>& RecordedPath = MoveRecordingComponent->GetActiveMoveAxialPath();
		if (RecordedPath.IsValidIndex(CurrentPathIndex))
		{
			StartAxial = RecordedPath[CurrentPathIndex];
		}
	}
	const FIntPoint TargetAxial = TargetTile->GetAxialCoord();

	if (!GridMap.Contains(StartAxial) || !GridMap.Contains(TargetAxial)) return RejectPlantWithFeedback();

	FML_TileNeighbors CurrentNeighbors;
	Board->GetNeighbors(MycelandCharacter->CurrentTileOn, CurrentNeighbors);
	if (!MoveRecordingComponent->IsMoveInProgress() && CurrentNeighbors.Contains(TargetTile))
	{
		TransitionComponent->StartTurnTowardTile(TargetTile);
		return true;
	}

	TArray<FIntPoint> FullPath;
	if (!UML_HexPathfinder::BuildPath_AxialBFS(StartAxial, TargetAxial, GridMap, FullPath)) return RejectPlantWithFeedback();
	if (FullPath.Num() < 2) return RejectPlantWithFeedback();

	const FIntPoint StopAxial = FullPath[FullPath.Num() - 2];
	if (!GridMap.Contains(StopAxial) || !UML_HexPathfinder::IsTileWalkable(GridMap[StopAxial])) return RejectPlantWithFeedback();

	AML_Tile* StopTile = GridMap[StopAxial];
	FML_TileNeighbors StopNeighbors;
	Board->GetNeighbors(StopTile, StopNeighbors);
	if (!StopNeighbors.Contains(TargetTile)) return RejectPlantWithFeedback();

	TArray<FIntPoint> MovePath = FullPath;
	MovePath.RemoveAt(MovePath.Num() - 1);
	if (MovePath.Num() == 0)
	{
		MovePath.Add(StartAxial);
	}

	if (!StartRecordedBoardMove(MovePath, GridMap, EML_PlayerBoardActionState::MovingToPlant, TargetTile))
	{
		return RejectPlantWithFeedback();
	}

	return true;
}

bool AML_PlayerController::RejectPlantWithFeedback()
{
	if (UML_SoundSubsystem* SoundSubsystem = UML_SoundSubsystem::Get(this))
	{
		SoundSubsystem->StartSound2DByPath(MLFMODEvents::TilePlacementInvalid);
	}

	return false;
}

void AML_PlayerController::ExecutePlant(AML_Tile* HitTile)
{
	EnergyComponent->AddEnergy(-1);

	if (UML_WavePropagationSubsystem* WavePropagationSubsystem = GetWorld()->GetSubsystem<UML_WavePropagationSubsystem>())
	{
		OnGrassPlanted.Broadcast(HitTile);
		WavePropagationSubsystem->BeginTileResolved(HitTile);
		if (UML_SoundSubsystem* SoundSubsystem = UML_SoundSubsystem::Get(this))
		{
			SoundSubsystem->StartSound2DByPath(
				MLFMODEvents::TileNaturePlaceSuccess
			);
		}
	}
}

// ==================== Movement - Path Management ====================

bool AML_PlayerController::StartRecordedBoardMove(const TArray<FIntPoint>& AxialPath, const TMap<FIntPoint, AML_Tile*>& GridMap,
	EML_PlayerBoardActionState ActionState, AML_Tile* PlantTarget)
{
	if (!IsValid(MycelandCharacter) || !IsValid(MycelandCharacter->CurrentTileOn)) return false;

	const bool bMoveAlreadyInProgress =
		MoveRecordingComponent->IsMoveInProgress() &&
		!MoveRecordingComponent->IsUndoMovePlayback();

	if ((!bMoveAlreadyInProgress && AxialPath.Num() < 2) || (bMoveAlreadyInProgress && AxialPath.Num() < 1))
		return false;

	const FIntPoint GoalAxial = AxialPath.Last();

	AML_Tile* const* TargetTilePtr = GridMap.Find(GoalAxial);
	if (!TargetTilePtr || !IsValid(*TargetTilePtr)) return false;

	if (bMoveAlreadyInProgress)
	{
		// Redirect from the waypoint currently being aimed at in the recorded path,
		// not from CurrentTileOn. That keeps the merged path valid even after many
		// consecutive redirects before the logical tile ownership updates.
		const TArray<FIntPoint>& RecordedPath = MoveRecordingComponent->GetActiveMoveAxialPath();
		if (RecordedPath.Num() < 2) return false;

		const int32 JunctionIndex = FMath::Clamp(CurrentPathIndex, 0, RecordedPath.Num() - 1);
		const FIntPoint JunctionAxial = RecordedPath[JunctionIndex];

		if (!GridMap.Contains(JunctionAxial) || !GridMap.Contains(GoalAxial)) return false;

		TArray<FIntPoint> RedirectSubPath;
		if (GoalAxial == JunctionAxial)
		{
			RedirectSubPath.Add(JunctionAxial);
		}
		else
		{
			if (!UML_HexPathfinder::BuildPath_AxialBFS(JunctionAxial, GoalAxial, GridMap, RedirectSubPath)) return false;
			if (RedirectSubPath.Num() < 2) return false;
		}

		const TArray<FIntPoint>& FullMergedPath = MoveRecordingComponent->ExtendMoveRecord(
			GoalAxial,
			(*TargetTilePtr)->GetActorLocation(),
			RedirectSubPath,
			JunctionIndex
		);

		// Rebuild the world path from the merged record and keep the same logical
		// target index, so repeated redirects stay stable.
		ExtendMoveAlongPath(FullMergedPath, GridMap, JunctionIndex);

		// Update the action state (e.g. Moving → MovingToPlant) if needed.
		TransitionComponent->SetBoardActionState(ActionState, PlantTarget);
	}
	else
	{
		// Fresh move — normal begin record.
		const FIntPoint StartAxial = MycelandCharacter->CurrentTileOn->GetAxialCoord();

		MoveRecordingComponent->BeginMoveRecord(
			StartAxial,
			GoalAxial,
			MycelandCharacter->GetActorLocation(),
			(*TargetTilePtr)->GetActorLocation(),
			AxialPath
		);

		TransitionComponent->SetBoardActionState(ActionState, PlantTarget);
		StartMoveAlongPath(AxialPath, GridMap);
	}

	return true;
}

void AML_PlayerController::ExtendMoveAlongPath(const TArray<FIntPoint>& FullMergedAxialPath,
                                               const TMap<FIntPoint, AML_Tile*>& GridMap,
                                               int32 PreservedPathIndex)
{
	// Rebuild the full world-space path from the merged axial path.
	TArray<FVector> NewPathWorld;
	NewPathWorld.Reserve(FullMergedAxialPath.Num());

	for (const FIntPoint& Axial : FullMergedAxialPath)
		if (AML_Tile* const* TilePtr = GridMap.Find(Axial))
			if (IsValid(*TilePtr))
				NewPathWorld.Add((*TilePtr)->GetActorLocation());

	if (NewPathWorld.Num() == 0)
		return; // Nothing valid — keep the current path as-is.

	// The merged axial path preserves the current logical target at the same index.
	// Keep that exact waypoint instead of trying to rediscover it from world positions.
	const FVector PlayerLoc = IsValid(MycelandCharacter) ? MycelandCharacter->GetActorLocation() : FVector::ZeroVector;
	int32 NewIndex = FMath::Clamp(PreservedPathIndex, 0, NewPathWorld.Num() - 1);

	// Advance past any waypoints the player has already reached.
	while (NewIndex < NewPathWorld.Num() &&
	       FVector::DistSquared2D(PlayerLoc, NewPathWorld[NewIndex]) <= FMath::Square(AcceptanceRadius))
	{
		NewIndex++;
	}

	if (NewIndex >= NewPathWorld.Num())
	{
		// Player is already at or past the new destination — treat as finished.
		CurrentPathWorld = MoveTemp(NewPathWorld);
		CurrentPathIndex = CurrentPathWorld.Num();
		return;
	}

	CurrentPathWorld  = MoveTemp(NewPathWorld);
	CurrentPathIndex  = NewIndex;
	// bIsMoving is already true; no need to call SetIsMoving again.
}

// ==================== Ground Validation ====================

bool AML_PlayerController::IsClickableGround(const FHitResult& Hit) const
{
	// Tracing on the Cursor channel (ECC_GameTraceChannel1) already guarantees that only intentional
	// surfaces (tile GroundBase + walkable ground) respond Block, so any blocking hit is a valid target.
	return Hit.bBlockingHit && Hit.Component.IsValid();
}

bool AML_PlayerController::GetGroundUnderCursor(FHitResult& OutHit) const
{
	// Trace on the dedicated Ground channel (ECC_GameTraceChannel2). Only designated ground
	// surfaces respond Block on it; decor ignores it by default (channel default = Ignore).
	if (!GetHitResultUnderCursorByChannel(UEngineTypes::ConvertToTraceType(ECC_GameTraceChannel2), false, OutHit))
		return false;
	if (!OutHit.bBlockingHit || !IsValid(OutHit.GetActor()))
		return false;

	// Tile GroundBase blocks ALL channels (see ML_TileBase), so board tiles also stop this trace —
	// a tile hit correctly reads as "cursor over the board", not over ground.
	return ExtractTileFromHit(OutHit) == nullptr;
}

// ==================== Camera Queries ====================

AML_CameraRail* AML_PlayerController::FindClosestCameraRailFromPlayer(const FVector& WorldLocation)
{
	AML_CameraRail* ClosestCameraRail = nullptr;
	float ClosestDistance = FLT_MAX;

	for (TActorIterator<AML_CameraRail> It(GetWorld()); It; ++It)
	{
		AML_CameraRail* CameraRail = *It;

		// Get the closest key from the LooAt spline from the WorldLocation parameter, then get the world location of this key
		float InputKey = CameraRail->GetLookAtFromCameraRail()->FindInputKeyClosestToWorldLocation(WorldLocation);
		FVector LookAtLocation = CameraRail->GetLookAtFromCameraRail()->GetLocationAtSplineInputKey(InputKey, ESplineCoordinateSpace::World);

		float Distance = FVector::DistSquared(WorldLocation, LookAtLocation);
		if (Distance < ClosestDistance)
		{
			ClosestDistance = Distance;
			ClosestCameraRail = CameraRail;
		}
	}

	return ClosestCameraRail;
}

// ==================== Delegates ====================

void AML_PlayerController::HandleCurrentTileChanged(const AML_Tile* OldTile, const AML_Tile* NewTile)
{
	if (HoverPreviewComponent)
		HoverPreviewComponent->NotifyPlayerTileChanged();
}

void AML_PlayerController::HandleBoardStateChanged(const AML_Tile* OldTile, const AML_Tile* NewTile)
{
	// ---------- Energy ----------
	if (NewTile)
		EnergyComponent->InitNumberOfEnergyForLevel(NewTile->GetBoardSpawnerFromTile()->GetEnergyForPuzzle());
	else
		EnergyComponent->InitNumberOfEnergyForLevel(0);

	// ---------- Automatic transition: Free ↔ InsideBoard ----------
	const bool bShouldBeInBoard = IsValid(NewTile);
	const bool bCurrentlyOutsideBoard = TransitionComponent->IsOutsideBoardMovementMode();

	// Per-board transition switch: if this board's transition is disabled, never auto-enter it —
	// stay in free movement even while physically standing on its tiles.
	const AML_BoardSpawner* NewBoard = NewTile ? NewTile->GetBoardSpawnerFromTile() : nullptr;
	const bool bBoardTransitionOn = !IsValid(NewBoard) || NewBoard->IsBoardTransitionEnabled();

	if (bShouldBeInBoard && bCurrentlyOutsideBoard && bBoardTransitionOn)
	{
		SetMovementMode(EML_PlayerMovementMode::InsideBoard);

		CurrentPathWorld.Reset();
		CurrentPathIndex = 0;

		if (!IsValid(MycelandCharacter))
			return;

		FVector TileCenter = NewTile->GetActorLocation();
		TileCenter.Z = MycelandCharacter->GetActorLocation().Z;

		CurrentPathWorld.Add(TileCenter);
		CurrentPathIndex = 0;
		SetIsMoving(true);
	}
	else if (!bShouldBeInBoard && !bCurrentlyOutsideBoard)
	{
		SetMovementMode(EML_PlayerMovementMode::FreeMovement);
	}
}

// ==================== Lifecycle ====================

void AML_PlayerController::BeginPlay()
{
	Super::BeginPlay();
	GetWorld()->GetSubsystem<UML_WavePropagationSubsystem>()->EnsureInitialized();
	GetWorld()->GetSubsystem<UML_RollBackSubsystem>()->EnsureInitialized();
	DevSettings = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();

	//ShowLoadingScreen();
}

void AML_PlayerController::ShowLoadingScreen()
{
	if (!LoadingScreenClass)
		return;

	LoadingScreenInstance = CreateWidget<UML_WidgetBase>(this, LoadingScreenClass);
	if (!LoadingScreenInstance)
		return;

	// High Z-order so it draws over everything (HUD, board, etc.) while the level settles on load.
	LoadingScreenInstance->AddToViewport(9999);

	// Block gameplay input so the player can't move (or plant, pan the camera, etc.) behind the
	// splash. Same lever the cinematic / rollback / board-action systems use. Restored in
	// HideLoadingScreen. Movement is also gated by bInCinematicMode in the handlers, so a narrative
	// cinematic that starts within the window keeps movement blocked even after we re-enable input.
	DisableInput(this);

	// Auto-hide after LoadingScreenDuration. A duration <= 0 keeps it up until HideLoadingScreen
	// is called manually (e.g. from Blueprint once real loading finishes).
	if (LoadingScreenDuration > 0.f)
	{
		GetWorldTimerManager().SetTimer(
			LoadingScreenTimerHandle, this, &AML_PlayerController::HideLoadingScreen,
			LoadingScreenDuration, /*bLoop=*/false);
	}
}

void AML_PlayerController::HideLoadingScreen()
{
	GetWorldTimerManager().ClearTimer(LoadingScreenTimerHandle);

	if (LoadingScreenInstance)
	{
		LoadingScreenInstance->RemoveFromParent();
		LoadingScreenInstance = nullptr;

		// Restore the gameplay input suspended in ShowLoadingScreen.
		EnableInput(this);
	}
}

void AML_PlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	// Only tick if moving
	if (bIsMoving)
		TickMoveAlongPath(DeltaTime);
}

void AML_PlayerController::OnPossess(APawn* aPawn)
{
	Super::OnPossess(aPawn);

	// BeginPlay sets DevSettings, but OnPossess can fire first (UE startup order is not guaranteed).
	if (!DevSettings)
		DevSettings = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();

	MycelandCharacter = Cast<AML_PlayerCharacter>(aPawn);
	if (MycelandCharacter)
	{
		MycelandCharacter->OnCurrentTileChanged.AddDynamic(this, &AML_PlayerController::HandleCurrentTileChanged);
		MycelandCharacter->OnBoardChanged.AddDynamic(this, &AML_PlayerController::HandleBoardStateChanged);
		HoverPreviewComponent->Initialize(this, MycelandCharacter);
		TransitionComponent->Initialize(this, MycelandCharacter, DevSettings, RotateSpeed);
		NavigationBridgeComponent->Initialize(this, MycelandCharacter, NavMeshAcceptanceRadius);

		MouseKeyboardHandler->Initialize(this);
		GamepadHandler->Initialize(this);
		InputDeviceManager->Initialize(MouseKeyboardHandler, GamepadHandler);
		InputDeviceManager->OnInputDeviceChanged.AddDynamic(this, &AML_PlayerController::HandleInputDeviceChanged);

		// Tell the device manager which devices actually have a gameplay IMC, so it never switches to a
		// device the designer intentionally left out of the array (single-IMC setups lock to one device).
		bool bMKAvailable = false;
		bool bGamepadAvailable = false;
		if (DevSettings)
		{
			for (const FML_InputMappingEntry& Entry : DevSettings->GameplayInputMappingContexts)
			{
				switch (Entry.Device)
				{
					case EML_InputMappingDevice::MouseKeyboard: bMKAvailable = true; break;
					case EML_InputMappingDevice::Gamepad:       bGamepadAvailable = true; break;
					case EML_InputMappingDevice::Both:          bMKAvailable = true; bGamepadAvailable = true; break;
				}
			}
		}
		InputDeviceManager->SetAvailableDevices(bMKAvailable, bGamepadAvailable);

		// Apply cursor visibility + map the gameplay IMCs (mouse/keyboard + gamepad).
		UpdateCursorVisibility(InputDeviceManager->GetCurrentDevice() == EML_InputDevice::MouseKeyboard);
		ApplyGameplayInputMappingContext();
		ApplyCheatToggleInputMappingContext();

		MycelandCharacter->UpdateCurrentTile();
		// Start inside the board only if the player stands on one AND that board's transition is enabled;
		// otherwise stay in free movement even while standing on board tiles.
		const AML_BoardSpawner* StartBoard = MycelandCharacter->CurrentTileOn
			? MycelandCharacter->CurrentTileOn->GetBoardSpawnerFromTile()
			: nullptr;
		const bool bStartInBoard = IsValid(StartBoard) && StartBoard->IsBoardTransitionEnabled();
		const EML_PlayerMovementMode InitialMode = bStartInBoard
			? EML_PlayerMovementMode::InsideBoard
			: EML_PlayerMovementMode::FreeMovement;
		TransitionComponent->SwitchToMode(InitialMode);

		// Board camera when spawning inside a board, closest rail otherwise. 0s: no blend on spawn.
		ApplyCameraForCurrentLocation(0.f);

		// Always start hover timer for cursor glow
		HoverPreviewComponent->StartHoverPreviewTimer();
	}
}

// ==================== Input ====================

void AML_PlayerController::OnSetDestinationStarted()
{
	if (bInCinematicMode) return;
	if (!InputDeviceManager) return;
	InputDeviceManager->NotifyMouseKeyboardInput();
	if (bShouldConsumeNextInput) return; // flag stays true until Released
	if (InputDeviceManager->GetActiveHandler())
		InputDeviceManager->GetActiveHandler()->OnMoveActionStarted();
}

void AML_PlayerController::OnSetDestinationTriggered()
{
	if (bInCinematicMode) return;
	if (bShouldConsumeNextInput) return;
	if (InputDeviceManager && InputDeviceManager->GetActiveHandler())
		InputDeviceManager->GetActiveHandler()->OnMoveActionTriggered(GetWorld()->GetDeltaSeconds());
}

void AML_PlayerController::OnSetDestinationReleased()
{
	// Reset the consume flag here — covers the full Started→Triggered→Released click cycle.
	// Do NOT call the handler: OnMoveActionStarted was skipped so HoldMoveCachedDestination
	// is stale, and calling OnMoveActionReleased would launch a navmesh move to a wrong target.
	if (bShouldConsumeNextInput) { bShouldConsumeNextInput = false; return; }
	if (bInCinematicMode) return;
	if (InputDeviceManager && InputDeviceManager->GetActiveHandler())
		InputDeviceManager->GetActiveHandler()->OnMoveActionReleased();
}

void AML_PlayerController::OnMoveAndPlantStarted()
{
	if (bInCinematicMode) return;
	if (!InputDeviceManager) return;
	InputDeviceManager->NotifyMouseKeyboardInput();
	if (bShouldConsumeNextInput) { bShouldConsumeNextInput = false; return; }
	if (InputDeviceManager->GetActiveHandler())
		InputDeviceManager->GetActiveHandler()->OnMoveAndPlantAction();
}

void AML_PlayerController::OnGamepadConfirmStarted()
{
	if (bInCinematicMode) return;
	if (!InputDeviceManager) return;
	// The gamepad IMC stays mapped even in MK mode, so this can fire while the MK handler is active:
	// switch to gamepad first so the action routes to the gamepad handler.
	InputDeviceManager->NotifyGamepadInput();
	if (InputDeviceManager->GetActiveHandler())
		InputDeviceManager->GetActiveHandler()->OnMoveAndPlantAction(); // Confirm = plant the selected tile
}

void AML_PlayerController::OnGamepadMoveAxis(const FVector2D& Value)
{
	if (bInCinematicMode) return;
	if (!InputDeviceManager) return;

	// OnInputHardwareDeviceChanged does not fire for analog axes, so we switch here directly.
	InputDeviceManager->NotifyGamepadInput();

	if (InputDeviceManager->GetActiveHandler())
		InputDeviceManager->GetActiveHandler()->OnStickAxis(Value, GetWorld()->GetDeltaSeconds());
}

void AML_PlayerController::OnGamepadMoveReleased()
{
	if (bInCinematicMode) return;
	if (InputDeviceManager && InputDeviceManager->GetActiveHandler())
		InputDeviceManager->GetActiveHandler()->OnStickReleased();
}

void AML_PlayerController::OnGamepadSelectPlantAxis(const FVector2D& Value)
{
	if (bInCinematicMode) return;
	if (!InputDeviceManager) return;

	// Analog axes don't raise OnInputHardwareDeviceChanged, so switch device explicitly here.
	InputDeviceManager->NotifyGamepadInput();

	if (InputDeviceManager->GetActiveHandler())
		InputDeviceManager->GetActiveHandler()->OnPlantSelectAxis(Value, GetWorld()->GetDeltaSeconds());
}

void AML_PlayerController::OnGamepadSelectPlantReleased()
{
	if (bInCinematicMode) return;
	if (InputDeviceManager && InputDeviceManager->GetActiveHandler())
		InputDeviceManager->GetActiveHandler()->OnPlantSelectReleased();
}

void AML_PlayerController::OnSkipNarrativeLine()
{
	if (UML_NarrativeSubsystem* SubSys = UML_NarrativeSubsystem::Get(this))
		SubSys->SkipCurrentLine();
}

// ==================== Camera ====================

void AML_PlayerController::ApplyCameraForCurrentLocation(float BlendTime)
{
	if (!IsValid(MycelandCharacter))
		return;

	AML_BoardSpawner* Board = IsValid(MycelandCharacter->CurrentTileOn)
		? MycelandCharacter->CurrentTileOn->GetBoardSpawnerFromTile()
		: nullptr;

	// Same rule as the movement mode: a board whose transition is disabled is never entered, so the
	// player keeps the free-movement camera even while physically standing on its tiles.
	if (IsValid(Board) && Board->IsBoardTransitionEnabled())
	{
		ACameraActor* BoardCamera = Board->GetAssociatedCamera();
		ensureMsgf(IsValid(BoardCamera), TEXT("No camera associated to board %s found. Make sure there is one associated."), *Board->GetName());

		if (IsValid(BoardCamera))
			BlendToViewTarget(BoardCamera, BlendTime);

		return;
	}

	AML_CameraRail* CameraRail = FindClosestCameraRailFromPlayer(MycelandCharacter->GetActorLocation());
	ensureMsgf(IsValid(CameraRail), TEXT("No camera rail found for player %s. Make sure there is one in the level."), *MycelandCharacter->GetName());

	if (IsValid(CameraRail))
		BlendToViewTarget(CameraRail, BlendTime);
}

void AML_PlayerController::BlendToViewTarget(AActor* NewViewTarget, float BlendTime, float BlendExp, EViewTargetBlendFunction BlendFunc)
{
	// Deactivate the tick of the old camera rail
	if (AML_CameraRail* OldRail = Cast<AML_CameraRail>(GetViewTarget()))
		OldRail->SetActorTickEnabled(false);

	// Activate the tick of the new camera rail
	if (AML_CameraRail* NewRail = Cast<AML_CameraRail>(NewViewTarget))
		NewRail->SetActorTickEnabled(true);

	// bLockOutgoing: if a blend is interrupted (e.g. re-entering a board mid-blend towards the
	// rail), start the new blend from the camera's current interpolated position instead of
	// re-evaluating the outgoing view target — otherwise blending back to the outgoing target
	// makes source == destination and the camera snaps instantly.
	SetViewTargetWithBlend(NewViewTarget, BlendTime, BlendFunc, BlendExp, true);
}

// ==================== Movement Control ====================

void AML_PlayerController::StartMoveAlongPath(const TArray<FIntPoint>& AxialPath,
                                              const TMap<FIntPoint, AML_Tile*>& GridMap)
{
	// Stop any nav mesh movement when starting tile-by-tile movement
	if (NavigationBridgeComponent)
		NavigationBridgeComponent->StopNavMeshMovement();

	CurrentPathWorld.Reset();
	CurrentPathIndex = 0;
	CurrentPathWorld.Reserve(AxialPath.Num());

	for (const FIntPoint& Axial : AxialPath)
		if (AML_Tile* const* TilePtr = GridMap.Find(Axial))
			if (IsValid(*TilePtr))
				CurrentPathWorld.Add((*TilePtr)->GetActorLocation());

	// If no valid path → not moving
	if (CurrentPathWorld.Num() == 0)
	{
		SetIsMoving(false);
		return;
	}

	// Skip first point if already there
	if (APawn* P = GetPawn())
		if (FVector::DistSquared2D(P->GetActorLocation(), CurrentPathWorld[0]) <= FMath::Square(AcceptanceRadius))
			CurrentPathIndex = 1;

	// If already at destination
	if (CurrentPathIndex >= CurrentPathWorld.Num())
	{
		CurrentPathWorld.Reset();
		CurrentPathIndex = 0;
		SetIsMoving(false);
		OnPathFinished();
		return;
	}

	SetIsMoving(true);
}

void AML_PlayerController::StartNavMeshMovement(const FVector& WorldLocation)
{
	if (NavigationBridgeComponent)
		NavigationBridgeComponent->StartNavMeshMovement(WorldLocation);
	SetIsMoving(true);
}

void AML_PlayerController::SetMovementMode(EML_PlayerMovementMode NewMode)
{
	if (TransitionComponent->GetMovementMode() == NewMode)
		return;

	const EML_PlayerMovementMode OldMode = TransitionComponent->GetMovementMode();

	// Let the component update its state and handle pending-state cleanup
	TransitionComponent->SwitchToMode(NewMode);

	// Notify hover system
	HoverPreviewComponent->NotifyMovementModeChanged(NewMode);

	// Controller-side reaction: stop NavMesh when entering the board
	if (NewMode == EML_PlayerMovementMode::InsideBoard &&
		(OldMode == EML_PlayerMovementMode::FreeMovement || OldMode == EML_PlayerMovementMode::EnteringBoard))
	{
		if (NavigationBridgeComponent)
			NavigationBridgeComponent->StopNavMeshMovement();
	}
}

// ==================== Callbacks & State Management ====================

void AML_PlayerController::NotifyGrassPlantStarted(AML_Tile* TargetTile)
{
	OnGrassPlantStarted.Broadcast(TargetTile);
}

void AML_PlayerController::ConfirmTurn(AML_Tile* HitTile)
{
	ExecutePlant(HitTile);
}

void AML_PlayerController::UpdateCursorVisibility(const bool bVisible)
{
	if (!IsLocalController())
		return;

	bShowMouseCursor = bVisible;
	bEnableClickEvents = bVisible;
	bEnableMouseOverEvents = bVisible;
}

UEnhancedInputLocalPlayerSubsystem* AML_PlayerController::GetEnhancedInputSubsystem() const
{
	if (!IsLocalController()) return nullptr;
	ULocalPlayer* LP = GetLocalPlayer();
	return LP ? LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
}

// ==================== Cheats ====================

void AML_PlayerController::ApplyCheatToggleInputMappingContext()
{
	const UML_MycelandDeveloperSettings* Settings = DevSettings
		? DevSettings
		: UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();

	if (!Settings || !Settings->bEnableCheats)
		return;

	UEnhancedInputLocalPlayerSubsystem* InputSub = GetEnhancedInputSubsystem();
	if (!InputSub)
		return;

	int32 Priority = 0;
	if (UInputMappingContext* ToggleIMC = Settings->GetInputMappingContext(EInputMappingType::CheatToggle, Priority))
		InputSub->AddMappingContext(ToggleIMC, Priority);
}

void AML_PlayerController::BindCheatActions(UEnhancedInputComponent* EIC)
{
	const UML_MycelandDeveloperSettings* Settings = DevSettings
		? DevSettings
		: UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();

	if (!Settings || !Settings->bEnableCheats || !EIC)
		return;

	if (UInputAction* Action = Settings->CheatToggleAction.LoadSynchronous())
		EIC->BindAction(Action, ETriggerEvent::Started, this, &AML_PlayerController::OnCheatToggle);

	if (UInputAction* Action = Settings->CheatTeleportSlotAction.LoadSynchronous())
		EIC->BindAction(Action, ETriggerEvent::Started, this, &AML_PlayerController::OnCheatTeleportSlot);

	if (UInputAction* Action = Settings->CheatLevelSlotAction.LoadSynchronous())
		EIC->BindAction(Action, ETriggerEvent::Started, this, &AML_PlayerController::OnCheatLevelSlot);

	if (UInputAction* Action = Settings->CheatWinPuzzleAction.LoadSynchronous())
		EIC->BindAction(Action, ETriggerEvent::Started, this, &AML_PlayerController::OnCheatWinPuzzle);

	if (UInputAction* Action = Settings->CheatInfiniteEnergyAction.LoadSynchronous())
		EIC->BindAction(Action, ETriggerEvent::Started, this, &AML_PlayerController::OnCheatInfiniteEnergy);

	if (UInputAction* Action = Settings->CheatExitBoardAction.LoadSynchronous())
		EIC->BindAction(Action, ETriggerEvent::Started, this, &AML_PlayerController::OnCheatExitBoard);

	if (UInputAction* Action = Settings->CheatSkipNarrativeAction.LoadSynchronous())
		EIC->BindAction(Action, ETriggerEvent::Started, this, &AML_PlayerController::OnCheatSkipNarrative);
}

void AML_PlayerController::OnCheatToggle()
{
	if (UML_CheatSubsystem* Cheats = UML_CheatSubsystem::Get(this))
		Cheats->ToggleCheatMode();
}

void AML_PlayerController::OnCheatTeleportSlot(const FInputActionValue& Value)
{
	// One Axis1D action covers all nine slots: each key mapping carries a Scalar modifier whose
	// value IS the slot number, so the pressed key arrives here as a plain 1..9.
	if (UML_CheatSubsystem* Cheats = UML_CheatSubsystem::Get(this))
		Cheats->Cheat_TeleportToSlot(FMath::RoundToInt(Value.Get<float>()));
}

void AML_PlayerController::OnCheatLevelSlot(const FInputActionValue& Value)
{
	if (UML_CheatSubsystem* Cheats = UML_CheatSubsystem::Get(this))
		Cheats->Cheat_OpenLevelSlot(FMath::RoundToInt(Value.Get<float>()));
}

void AML_PlayerController::OnCheatWinPuzzle()
{
	if (UML_CheatSubsystem* Cheats = UML_CheatSubsystem::Get(this))
		Cheats->Cheat_WinCurrentPuzzle();
}

void AML_PlayerController::OnCheatInfiniteEnergy()
{
	if (UML_CheatSubsystem* Cheats = UML_CheatSubsystem::Get(this))
		Cheats->Cheat_ToggleInfiniteEnergy();
}

void AML_PlayerController::OnCheatExitBoard()
{
	if (UML_CheatSubsystem* Cheats = UML_CheatSubsystem::Get(this))
		Cheats->Cheat_ExitBoard();
}

void AML_PlayerController::OnCheatSkipNarrative()
{
	if (UML_CheatSubsystem* Cheats = UML_CheatSubsystem::Get(this))
		Cheats->Cheat_SkipNarrative();
}

void AML_PlayerController::ApplyGameplayInputMappingContext()
{
	UEnhancedInputLocalPlayerSubsystem* InputSub = GetEnhancedInputSubsystem();
	if (!InputSub || !DevSettings) return;

	// Map every configured gameplay IMC together (mouse/keyboard, gamepad, or both — any combination).
	// Adding an already-mapped context is a no-op, so this is safe to call repeatedly.
	for (const FML_InputMappingEntry& Entry : DevSettings->GameplayInputMappingContexts)
		if (UInputMappingContext* IMC = Entry.Mapping.LoadSynchronous())
			InputSub->AddMappingContext(IMC, Entry.Priority);
}

void AML_PlayerController::RemoveGameplayInputMappingContexts()
{
	UEnhancedInputLocalPlayerSubsystem* InputSub = GetEnhancedInputSubsystem();
	if (!InputSub || !DevSettings) return;

	for (const FML_InputMappingEntry& Entry : DevSettings->GameplayInputMappingContexts)
		if (UInputMappingContext* IMC = Entry.Mapping.LoadSynchronous())
			InputSub->RemoveMappingContext(IMC);
}

void AML_PlayerController::NotifyCinematicModeChanged(const bool bNewInCinematicMode)
{
	bInCinematicMode = bNewInCinematicMode;
	if (bInCinematicMode)
	{
		UpdateCursorVisibility(false);
	}
	else
	{
		// Restore click events disabled by the cinematic, then apply device-appropriate state.
		bEnableClickEvents = true;
		const EML_InputDevice Device = InputDeviceManager
			? InputDeviceManager->GetCurrentDevice()
			: EML_InputDevice::MouseKeyboard;
		HandleInputDeviceChanged(Device);
	}
	if (HoverPreviewComponent)
		HoverPreviewComponent->UpdateShowPreviews(!bInCinematicMode);
}

void AML_PlayerController::HandleInputDeviceChanged(EML_InputDevice NewDevice)
{
	if (bInCinematicMode) return;

	const bool bIsMK = (NewDevice == EML_InputDevice::MouseKeyboard);

	if (!bIsMK && PreviousInputDevice == EML_InputDevice::MouseKeyboard)
	{
		// MK → Gamepad: save cursor position so it reappears in the same spot later.
		float X, Y;
		GetMousePosition(X, Y);
		LockedCursorPos = FVector2D(X, Y);
	}

	// Gamepad → MK transition: cursor is about to reappear.
	// The click that triggered this switch must not be acted on — it serves only to show the cursor.
	if (bIsMK && PreviousInputDevice == EML_InputDevice::Gamepad)
	{
		bShouldConsumeNextInput = true;
		// Restore cursor to where it was when the player switched to gamepad.
		SetMouseLocation(static_cast<int>(LockedCursorPos.X), static_cast<int>(LockedCursorPos.Y));
	}

	PreviousInputDevice    = NewDevice;
	bShowMouseCursor       = bIsMK;
	bEnableMouseOverEvents = bIsMK;

	// No IMC swap here: both device IMCs stay mapped (see ApplyGameplayInputMappingContext) so device
	// detection keeps working. A device switch only changes cursor visibility / hover behavior.

	if (HoverPreviewComponent)
		HoverPreviewComponent->NotifyInputDeviceChanged(NewDevice);

	if (TransitionComponent)
		TransitionComponent->NotifyInputDeviceChanged(NewDevice);

	// Force Slate to re-evaluate the cursor type immediately.
	// Without this, the OS cursor only updates on the next mouse-move event (Standalone artifact).
	if (FSlateApplication::IsInitialized())
		FSlateApplication::Get().QueryCursor();
}

// ==================== Actions ====================

bool AML_PlayerController::MovePlayerToAxial(const FIntPoint& TargetAxial, bool bUsePath, bool bFallbackTeleport,
                                             const FVector& TeleportFallbackWorld)
{
	if (!IsValid(MycelandCharacter) || !IsValid(MycelandCharacter->CurrentTileOn)) return false;

	AML_BoardSpawner* Board = MycelandCharacter->CurrentTileOn->GetBoardSpawnerFromTile();
	if (!IsValid(Board)) return false;

	const TMap<FIntPoint, AML_Tile*> GridMap = Board->GetGridMap();
	const FIntPoint StartAxial = MycelandCharacter->CurrentTileOn->GetAxialCoord();

	if (!GridMap.Contains(StartAxial) || !GridMap.Contains(TargetAxial))
	{
		if (bFallbackTeleport)
			MycelandCharacter->SetActorLocation(TeleportFallbackWorld);
		return false;
	}

	if (!bUsePath)
	{
		if (AML_Tile* const* TilePtr = GridMap.Find(TargetAxial))
		{
			if (IsValid(*TilePtr))
			{
				MycelandCharacter->SetActorLocation((*TilePtr)->GetActorLocation());
				return true;
			}
		}
		return false;
	}

	TArray<FIntPoint> AxialPath;
	if (!UML_HexPathfinder::BuildPath_AxialBFS(StartAxial, TargetAxial, GridMap, AxialPath))
	{
		if (bFallbackTeleport)
			MycelandCharacter->SetActorLocation(TeleportFallbackWorld);
		return false;
	}

	return StartRecordedBoardMove(AxialPath, GridMap);
}

void AML_PlayerController::StartMoveAlongAxialPathForUndo(const TArray<FIntPoint>& AxialPath,
                                                          const TArray<FIntPoint>& PickedCollectibleAxials)
{
	if (!IsValid(MycelandCharacter) || !IsValid(MycelandCharacter->CurrentTileOn)) return;

	AML_BoardSpawner* Board = MycelandCharacter->CurrentTileOn->GetBoardSpawnerFromTile();
	if (!IsValid(Board)) return;

	const TMap<FIntPoint, AML_Tile*> GridMap = Board->GetGridMap();

	MoveRecordingComponent->BeginUndoPlayback(AxialPath, PickedCollectibleAxials);
	StartMoveAlongPath(AxialPath, GridMap);
}

void AML_PlayerController::NotifyCollectiblePickedOnAxial(const FIntPoint& Axial)
{
	MoveRecordingComponent->NotifyCollectiblePicked(Axial);
}

// ==================== Component Wrappers ====================

void AML_PlayerController::RequestExitHold(AML_Tile* ExitBorderTile, const FVector& WorldTarget)
{
	if (TransitionComponent) TransitionComponent->RequestExitHold(ExitBorderTile, WorldTarget);
}

void AML_PlayerController::CancelExitHold()
{
	if (TransitionComponent) TransitionComponent->CancelExitHold();
}

void AML_PlayerController::RequestBoardEntry(AML_Tile* TargetTile)
{
	if (TransitionComponent) TransitionComponent->RequestBoardEntry(TargetTile);
}

bool AML_PlayerController::GetNavSteeringDirection(const FVector& Destination, float DeltaTime, FVector& OutDirection)
{
	OutDirection = FVector::ZeroVector;
	return NavigationBridgeComponent
		&& NavigationBridgeComponent->GetNavSteeringDirection(Destination, DeltaTime, OutDirection);
}

void AML_PlayerController::ResetNavSteering()
{
	if (NavigationBridgeComponent) NavigationBridgeComponent->ResetHoldSteering();
}

void AML_PlayerController::StopNavMeshMovement()
{
	if (NavigationBridgeComponent) NavigationBridgeComponent->StopNavMeshMovement();
}

void AML_PlayerController::CancelPendingNavigation()
{
	StopNavMeshMovement();
	if (TransitionComponent)
		TransitionComponent->CancelPendingBoardEntry();
}

void AML_PlayerController::CancelAllMovementForTeleport()
{
	CancelPendingNavigation();

	// Also drop the tile-by-tile board path: CancelPendingNavigation only covers the navmesh side,
	// and a leftover board path would keep being ticked toward its old destination.
	CurrentPathWorld.Reset();
	CurrentPathIndex = 0;
	SetIsMoving(false);

	// A hold-to-leave-the-board in progress would keep ticking against the border tile we just
	// teleported away from.
	CancelExitHold();

	ClearPathHoverPreview();
	ClearForcedHoverTile();
}

AML_Tile* AML_PlayerController::FindReachableExitBorderTile(const AML_BoardSpawner* Board, const FVector& OutsideDestination) const
{
	return NavigationBridgeComponent ? NavigationBridgeComponent->FindReachableExitBorderTile(Board, OutsideDestination) : nullptr;
}

AML_Tile* AML_PlayerController::PredictNavMeshEntryTile(const AML_BoardSpawner* Board, const FVector& Destination) const
{
	return NavigationBridgeComponent ? NavigationBridgeComponent->PredictNavMeshEntryTile(Board, Destination) : nullptr;
}

void AML_PlayerController::SetForcedHoverTile(AML_Tile* Tile)
{
	if (HoverPreviewComponent) HoverPreviewComponent->SetForcedHoverTile(Tile);
}

void AML_PlayerController::ClearForcedHoverTile()
{
	if (HoverPreviewComponent) HoverPreviewComponent->ClearForcedHoverTile();
}

void AML_PlayerController::SetGamepadSelectedTile(AML_Tile* Tile)
{
	if (HoverPreviewComponent) HoverPreviewComponent->SetGamepadSelectedTile(Tile);
}

AML_Tile* AML_PlayerController::GetGamepadSelectedTile() const
{
	return HoverPreviewComponent ? HoverPreviewComponent->GetGamepadSelectedTile() : nullptr;
}

void AML_PlayerController::ClearPathHoverPreview()
{
	if (HoverPreviewComponent) HoverPreviewComponent->ClearPathHoverPreview();
}

void AML_PlayerController::ClearActiveGlow()
{
	if (HoverPreviewComponent) HoverPreviewComponent->ClearActiveGlow();
}

void AML_PlayerController::NotifyBoardTransitionDisabled(const AML_BoardSpawner* Board)
{
	if (!TransitionComponent || !IsValid(MycelandCharacter) || !IsValid(MycelandCharacter->CurrentTileOn))
		return;

	// Only act if the player is currently on THIS board and in a board movement mode.
	if (MycelandCharacter->CurrentTileOn->GetBoardSpawnerFromTile() != Board)
		return;
	if (TransitionComponent->IsOutsideBoardMovementMode())
		return;

	// Stop any in-progress tile-by-tile movement, then eject to free movement.
	CurrentPathWorld.Reset();
	CurrentPathIndex = 0;
	SetIsMoving(false);
	TransitionComponent->ForceFreeMovement();
}

// ==================== Tile Query ====================

AML_Tile* AML_PlayerController::GetTileUnderCursor() const
{
	FHitResult Hit;
	// Trace on the dedicated Cursor channel (ECC_GameTraceChannel1) in SIMPLE collision.
	// Only the tile GroundBase and the walkable ground respond Block to this channel; decorative
	// meshes ignore it by default. bTraceComplex=false so tiles are picked via their simple collision
	// (per-poly collision is often absent, e.g. water, which would otherwise let the trace fall through
	// to the landscape and wrongly read as "no tile").
	if (!GetHitResultUnderCursorByChannel(UEngineTypes::ConvertToTraceType(ECC_GameTraceChannel1), false, Hit))
		return nullptr;

	// ExtractTileFromHit returns null when the hit is the open ground (not a tile) — that correctly
	// reads as "cursor outside the board".
	return ExtractTileFromHit(Hit);
}

AML_Tile* AML_PlayerController::ExtractTileFromHit(const FHitResult& Hit)
{
	if (AML_Tile* Tile = Cast<AML_Tile>(Hit.GetActor()))
		return Tile;

	if (UPrimitiveComponent* Comp = Hit.GetComponent())
		if (AML_Tile* OuterTile = Comp->GetTypedOuter<AML_Tile>())
			return OuterTile;

	if (AActor* HitActor = Hit.GetActor())
	{
		if (AActor* Parent = HitActor->GetParentActor())
			if (AML_Tile* ParentTile = Cast<AML_Tile>(Parent))
				return ParentTile;

		if (AActor* Owning = HitActor->GetOwner())
			if (AML_Tile* OwnerTile = Cast<AML_Tile>(Owning))
				return OwnerTile;
	}

	return nullptr;
}
