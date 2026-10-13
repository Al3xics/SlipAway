// Copyright Myceland Team, All Rights Reserved.

#include "Component/ML_BoardTransitionComponent.h"

#include "Core/ML_TileTypeTraits.h"
#include "Developer Settings/ML_MycelandDeveloperSettings.h"
#include "Player/ML_HexPathfinder.h"
#include "Player/ML_PlayerCharacter.h"
#include "Player/ML_PlayerController.h"
#include "Tiles/ML_BoardSpawner.h"
#include "Tiles/ML_Tile.h"

// ==================== Lifecycle ====================

void UML_BoardTransitionComponent::Initialize(AML_PlayerController* Controller, AML_PlayerCharacter* Character, const UML_MycelandDeveloperSettings* Settings, float InRotateSpeed)
{
	OwningController = Controller;
	PlayerCharacter  = Character;
	DevSettings      = Settings;
	RotateSpeed      = InRotateSpeed;
}

float UML_BoardTransitionComponent::GetCursorTickInterval() const
{
	return IsValid(DevSettings) ? DevSettings->GetCursorDetectionTickInterval() : 1.f / 30.f;
}

float UML_BoardTransitionComponent::GetExitHoldTickInterval() const
{
	return IsValid(DevSettings) ? DevSettings->GetExitHoldTickInterval() : 1.f / 60.f;
}

float UML_BoardTransitionComponent::GetTurnTickInterval() const
{
	return IsValid(DevSettings) ? DevSettings->GetTurnTowardTileTickInterval() : 1.f / 60.f;
}

// ==================== Mode management ====================

void UML_BoardTransitionComponent::SwitchToMode(EML_PlayerMovementMode NewMode)
{
	const EML_PlayerMovementMode OldMode = CurrentMovementMode;
	CurrentMovementMode = NewMode;

	// Ground detection only runs while the cursor can leave the board (InsideBoard/ExitingBoard).
	const bool bBoardMode = NewMode == EML_PlayerMovementMode::InsideBoard ||
	                        NewMode == EML_PlayerMovementMode::ExitingBoard;
	if (bBoardMode)
		StartGroundHoverDetection();
	else
		StopGroundHoverDetection();

	// ExitingBoard → InsideBoard: this is a CANCELLED exit — clean up exit state
	if (OldMode == EML_PlayerMovementMode::ExitingBoard &&
		NewMode  == EML_PlayerMovementMode::InsideBoard &&
		!bPendingFreeMovementOnArrival)
	{
		PendingExitBorderTile = nullptr;
		bHasExitTargetWorld = false;
	}
}

void UML_BoardTransitionComponent::NotifyIsMoving(bool bIsMoving)
{
	// Only broadcast when inside the board
	if (CurrentMovementMode != EML_PlayerMovementMode::InsideBoard)
	{
		bWasMovingInBoard = false;
		return;
	}

	if (bIsMoving != bWasMovingInBoard)
	{
		OnBoardActivityStateChanged.Broadcast(bIsMoving);
		bWasMovingInBoard = bIsMoving;
	}
}

bool UML_BoardTransitionComponent::IsOutsideBoardMovementMode() const
{
	return CurrentMovementMode == EML_PlayerMovementMode::FreeMovement ||
		CurrentMovementMode == EML_PlayerMovementMode::EnteringBoard;
}

void UML_BoardTransitionComponent::ForceFreeMovement()
{
	// Clear the exit-hold timer + state (SetMovementMode alone would leak the running timer).
	if (UWorld* World = GetWorld())
		World->GetTimerManager().ClearTimer(ExitHoldTimerHandle);

	if (bIsHoldingExitInput || bWasExitingLastFrame)
		OnExitCursorHold.Broadcast(false, 0.f);

	ExitHoldTimer         = 0.f;
	bIsHoldingExitInput   = false;
	bWasExitingLastFrame  = false;
	bHasExitTargetWorld   = false;
	LastBroadcastProgress = -1.f;
	PendingExitBorderTile = nullptr;

	// Clear pending board-entry state.
	bPendingBoardEntryOnArrival   = false;
	PendingBoardEntryTargetTile   = nullptr;
	bPendingFreeMovementOnArrival = false;

	OwningController->SetMovementMode(EML_PlayerMovementMode::FreeMovement);
}

// ==================== Exit hold ====================

void UML_BoardTransitionComponent::RequestExitHold(AML_Tile* ExitBorderTile, const FVector& WorldTarget)
{
	PendingExitBorderTile = ExitBorderTile;
	PendingExitTargetWorld = WorldTarget;
	bHasExitTargetWorld    = true;

	OwningController->SetMovementMode(EML_PlayerMovementMode::ExitingBoard);
	bIsHoldingExitInput = true;

	// Start exit hold timer
	ExitHoldTimer        = 0.f;
	bWasExitingLastFrame = false;
	LastBroadcastProgress = -1.f;

	GetWorld()->GetTimerManager().SetTimer(
		ExitHoldTimerHandle,
		this,
		&UML_BoardTransitionComponent::TickExitHold,
		GetExitHoldTickInterval(),
		true
	);
}

void UML_BoardTransitionComponent::CancelExitHold()
{
	bIsHoldingExitInput = false;
	// TickExitHold detects the cleared flag on the next tick and performs cleanup.
}

void UML_BoardTransitionComponent::TickExitHold()
{
	if (!IsValid(OwningController) || !IsValid(DevSettings))
	{
		GetWorld()->GetTimerManager().ClearTimer(ExitHoldTimerHandle);
		bIsHoldingExitInput = false;
		return;
	}

	const bool bIsCurrentlyExiting = (CurrentMovementMode == EML_PlayerMovementMode::ExitingBoard && bIsHoldingExitInput);

	// Not exiting anymore — cancellation
	if (!bIsCurrentlyExiting)
	{
		if (CurrentMovementMode == EML_PlayerMovementMode::ExitingBoard)
		{
			GetWorld()->GetTimerManager().ClearTimer(ExitHoldTimerHandle);
			ExitHoldTimer = 0.f;

			OnExitCursorHold.Broadcast(false, 0.0f);
			bWasExitingLastFrame  = false;
			LastBroadcastProgress = -1.f;

			// Hover cleanup on cancel is the input handler's responsibility (mouse clears, gamepad keeps).

			// Return to board — SwitchToMode handles clearing exit state
			OwningController->SetMovementMode(EML_PlayerMovementMode::InsideBoard);
		}
		return;
	}

	// Calculate progress. A zero (or negative) hold duration means "no hold" — a simple press
	// exits immediately, so progress is already full on the first tick (guards against div-by-zero).
	// Mouse and gamepad have separate hold durations (they share the tick rate). OwningController is
	// guaranteed valid here (checked at the top of TickExitHold).
	const float HoldDuration = OwningController->IsGamepadActive() ? DevSettings->ExitBoardHoldDurationGamepad : DevSettings->ExitBoardHoldDurationMouse;
	const float Progress = (HoldDuration > KINDA_SMALL_NUMBER)
		? FMath::Clamp(ExitHoldTimer / HoldDuration, 0.f, 1.f)
		: 1.f;

	const bool bJustStartedExiting = !bWasExitingLastFrame;
	const bool bProgressChanged    = FMath::Abs(Progress - LastBroadcastProgress) > 0.01f;

	if (bJustStartedExiting || bProgressChanged)
	{
		OnExitCursorHold.Broadcast(true, Progress);
		LastBroadcastProgress = Progress;
	}

	bWasExitingLastFrame = true;
	// Must match the timer rate: the hold progress advances by exactly one tick interval per tick.
	ExitHoldTimer += GetExitHoldTickInterval();

	if (ExitHoldTimer >= HoldDuration)
	{
		GetWorld()->GetTimerManager().ClearTimer(ExitHoldTimerHandle);
		ExitHoldTimer         = 0.f;
		bWasExitingLastFrame  = false;
		LastBroadcastProgress = -1.f;

		OnExitCursorHold.Broadcast(false, 1.0f);

		// The hold is fulfilled: clear the holding flag before starting the walk-out. From here the player is
		// carried off the board by the navmesh (see ConfirmExitBoard) while staying in ExitingBoard, and the
		// stick/cursor must no longer run the exit-cancel logic (HandleExitingBoardStick / TickGroundHover).
		bIsHoldingExitInput = false;

		OwningController->ClearForcedHoverTile();
		ConfirmExitBoard();
	}
}

void UML_BoardTransitionComponent::ConfirmExitBoard()
{
	UE_LOG(LogTemp, Warning, TEXT("[EXIT] ConfirmExitBoard called"));

	if (!IsValid(PendingExitBorderTile) || !IsValid(PlayerCharacter) || !IsValid(PlayerCharacter->CurrentTileOn)) return;

	AML_BoardSpawner* Board = PlayerCharacter->CurrentTileOn->GetBoardSpawnerFromTile();
	if (!IsValid(Board)) return;

	const TMap<FIntPoint, AML_Tile*> GridMap = Board->GetGridMap();
	const FIntPoint StartAxial = PlayerCharacter->CurrentTileOn->GetAxialCoord();
	const FIntPoint GoalAxial  = PendingExitBorderTile->GetAxialCoord();

	UE_LOG(LogTemp, Warning, TEXT("[EXIT] StartAxial=%s, GoalAxial=%s"),
		*StartAxial.ToString(), *GoalAxial.ToString());

	if (StartAxial == GoalAxial)
	{
		UE_LOG(LogTemp, Warning, TEXT("[EXIT] Direct exit (already on border tile)"));

		if (bHasExitTargetWorld)
		{
			// Stay in ExitingBoard while the navmesh carries the player off the board. The switch to
			// FreeMovement happens in AML_PlayerController::HandleBoardStateChanged the moment the player
			// physically leaves the board footprint (board -> null). Until then no free-movement input is
			// accepted (in ExitingBoard the stick routes to HandleExitingBoardStick and mouse clicks are
			// ignored), so the player can't hijack the walk-out and end up free-moving while still on the
			// board's tiles. TickMoveAlongPath force-switches to FreeMovement if the walk-out ever finishes
			// without leaving the footprint (e.g. an exit plane placed on a border tile).
			OwningController->StartNavMeshMovement(PendingExitTargetWorld);
			bHasExitTargetWorld = false;
		}
		else
		{
			// No target to walk to — nothing would carry the player out, so leave immediately.
			OwningController->SetMovementMode(EML_PlayerMovementMode::FreeMovement);
		}

		PendingExitBorderTile = nullptr;
		return;
	}

	if (!GridMap.Contains(StartAxial) || !GridMap.Contains(GoalAxial)) return;

	TArray<FIntPoint> AxialPath;
	if (!UML_HexPathfinder::BuildPath_AxialBFS(StartAxial, GoalAxial, GridMap, AxialPath)) return;

	UE_LOG(LogTemp, Warning, TEXT("[EXIT] Path to border. Length=%d, bHasExitTargetWorld=%d"),
		AxialPath.Num(), bHasExitTargetWorld);

	bPendingFreeMovementOnArrival = true;
	// Walk to the border tile-by-tile inside the board. On arrival HandlePathFinished switches to ExitingBoard
	// and navmeshes off the board; FreeMovement then triggers once the player leaves the board footprint.
	OwningController->SetMovementMode(EML_PlayerMovementMode::InsideBoard);
	// Don't clear pending exit state: HandlePathFinished needs PendingExitTargetWorld.

	OwningController->StartMoveAlongPath(AxialPath, GridMap);
}

// ==================== Ground hover (cursor outside-board detection) ====================

void UML_BoardTransitionComponent::StartGroundHoverDetection()
{
	if (!bCursorGroundDetectionEnabled || GroundHoverTimerHandle.IsValid()) return;

	GetWorld()->GetTimerManager().SetTimer(
		GroundHoverTimerHandle,
		this,
		&UML_BoardTransitionComponent::TickGroundHover,
		GetCursorTickInterval(), // Same rate as the hover preview — enough for cursor detection
		true
	);
}

void UML_BoardTransitionComponent::StopGroundHoverDetection()
{
	if (UWorld* World = GetWorld())
		World->GetTimerManager().ClearTimer(GroundHoverTimerHandle);

	SetHoveredGroundActor(nullptr);
}

void UML_BoardTransitionComponent::TickGroundHover()
{
	if (!IsValid(OwningController)) return;

	FHitResult Hit;
	AActor* NewGround = OwningController->GetGroundUnderCursor(Hit) ? Hit.GetActor() : nullptr;
	SetHoveredGroundActor(NewGround);

	// Exit hold in progress but the cursor left the designated ground (moved back over the board,
	// onto decor, or off any ground): cancel the exit. TickExitHold sees the cleared flag on its
	// next tick and performs the state cleanup + mode switch back to InsideBoard.
	if (bIsHoldingExitInput && !IsValid(HoveredGroundActor))
	{
		CancelExitHold();
		// The mouse release handler only clears the exit glow when the hold is still active on
		// release, so an auto-cancelled hold must clear it here.
		OwningController->ClearForcedHoverTile();
		OwningController->ClearPathHoverPreview();
	}
}

void UML_BoardTransitionComponent::SetHoveredGroundActor(AActor* NewGround)
{
	if (HoveredGroundActor == NewGround) return;

	HoveredGroundActor = NewGround;
	OnHoveredGroundChanged.Broadcast(NewGround);
}

void UML_BoardTransitionComponent::NotifyInputDeviceChanged(EML_InputDevice NewDevice)
{
	bCursorGroundDetectionEnabled = (NewDevice == EML_InputDevice::MouseKeyboard);

	if (!bCursorGroundDetectionEnabled) // --> if using gamepad, stop ground hover detection
		StopGroundHoverDetection();
	else if (CurrentMovementMode == EML_PlayerMovementMode::InsideBoard || CurrentMovementMode == EML_PlayerMovementMode::ExitingBoard)
		StartGroundHoverDetection();
}

// ==================== Board entry / action state ====================

void UML_BoardTransitionComponent::SetBoardActionState(EML_PlayerBoardActionState State, AML_Tile* PlantTarget)
{
	BoardActionState       = State;
	PendingPlantTargetTile = PlantTarget;

	// Reaching here means the controller just (re)started an in-board move via StartRecordedBoardMove —
	// either a fresh Move/Plant, or a redirect of a move already in progress (e.g. planting a tile while
	// still walking toward a previously-requested exit border tile). That walk is no longer the exit walk
	// started by ConfirmExitBoard, so any leftover "exit on arrival" intent must be cancelled here.
	// Without this, HandlePathFinished (Case 1) would mistake this path's completion for having reached
	// the exit border tile and send the player walking back off the board via navmesh — even though the
	// player just asked to move or plant elsewhere on the board. ConfirmExitBoard itself never calls this
	// function (it drives the exit walk directly via StartMoveAlongPath), so clearing here never disturbs
	// a genuine exit in progress.
	bPendingFreeMovementOnArrival = false;
	bHasExitTargetWorld           = false;
	PendingExitBorderTile         = nullptr;
}

void UML_BoardTransitionComponent::RequestBoardEntry(AML_Tile* TargetTile)
{
	// Per-board transition switch: if the target board has entry/exit disabled, do nothing.
	// The NavMesh move issued by the caller still runs, so the player walks to the tile in free movement.
	const AML_BoardSpawner* Board = IsValid(TargetTile) ? TargetTile->GetBoardSpawnerFromTile() : nullptr;
	// A world board is never "entered" either: the player just walks onto it in free movement.
	if (IsValid(Board) && !Board->ShouldStopPlayerOnEntry()) return;

	PendingBoardEntryTargetTile = TargetTile;
	bPendingBoardEntryOnArrival = true;
	OwningController->SetMovementMode(EML_PlayerMovementMode::EnteringBoard);
}

void UML_BoardTransitionComponent::CancelPendingBoardEntry()
{
	if (!bPendingBoardEntryOnArrival) return;

	bPendingBoardEntryOnArrival = false;
	PendingBoardEntryTargetTile = nullptr;

	if (CurrentMovementMode == EML_PlayerMovementMode::EnteringBoard)
		OwningController->SetMovementMode(EML_PlayerMovementMode::FreeMovement);
}

// ==================== Turn toward tile ====================

void UML_BoardTransitionComponent::StartTurnTowardTile(AML_Tile* Target)
{
	PendingPlantTargetTile = Target;
	BoardActionState       = EML_PlayerBoardActionState::TurningToPlant;
	OnBoardActivityStateChanged.Broadcast(true);

	// Tell Blueprint that the planting sequence has started.
	// Character begins rotating at the same time.
	if (IsValid(OwningController))
	{
		OwningController->NotifyGrassPlantStarted(Target);
	}

	if (!TurnTowardTileTimerHandle.IsValid())
	{
		GetWorld()->GetTimerManager().SetTimer(
			TurnTowardTileTimerHandle,
			this,
			&UML_BoardTransitionComponent::UpdateTurnTowardPendingTile,
			GetTurnTickInterval(),
			true
		);
	}
}

bool UML_BoardTransitionComponent::RotateCharacterTowardTile(const AML_Tile* Target, float DeltaTime, float TurnSpd) const
{
	if (!IsValid(PlayerCharacter) || !IsValid(Target)) return false;

	FVector Dir = Target->GetActorLocation() - PlayerCharacter->GetActorLocation();
	Dir.Z = 0.f;

	if (Dir.IsNearlyZero())
		return false;

	const float DesiredYaw      = Dir.Rotation().Yaw;
	const FRotator CurrentRot   = PlayerCharacter->GetActorRotation();
	const FRotator DesiredRot(0.f, DesiredYaw, 0.f);
	const FRotator NewRot       = FMath::RInterpTo(CurrentRot, DesiredRot, DeltaTime, TurnSpd);

	PlayerCharacter->SetActorRotation(NewRot);

	const float YawError = FMath::Abs(FMath::FindDeltaAngleDegrees(NewRot.Yaw, DesiredYaw));
	return YawError > 1.0f; // true = still turning
}

void UML_BoardTransitionComponent::UpdateTurnTowardPendingTile()
{
	if (BoardActionState != EML_PlayerBoardActionState::TurningToPlant ||
		!IsValid(PendingPlantTargetTile) || !IsValid(PlayerCharacter))
	{
		GetWorld()->GetTimerManager().ClearTimer(TurnTowardTileTimerHandle);
		BoardActionState = EML_PlayerBoardActionState::Idle;
		return;
	}

	// DeltaTime must match the timer rate so the RInterpTo rotation speed stays framerate-independent.
	const bool bStillTurning = RotateCharacterTowardTile(PendingPlantTargetTile, GetTurnTickInterval(), RotateSpeed);

	if (!bStillTurning)
	{
		GetWorld()->GetTimerManager().ClearTimer(TurnTowardTileTimerHandle);
		BoardActionState = EML_PlayerBoardActionState::Idle;
		OnBoardActivityStateChanged.Broadcast(false);

		AML_Tile* Target = PendingPlantTargetTile;
		PendingPlantTargetTile = nullptr;

		if (OwningController->HasEnergy())
			OwningController->ConfirmTurn(Target);
	}
}

// ==================== Path completion ====================

FBoardTransitionCommand UML_BoardTransitionComponent::HandlePathFinished(AML_PlayerCharacter* Character)
{
	FBoardTransitionCommand Cmd;

	// Case 1: We just walked to the board border — switch to free movement
	if (bPendingFreeMovementOnArrival)
	{
		UE_LOG(LogTemp, Warning, TEXT("[EXIT] OnPathFinished: bPendingFreeMovementOnArrival=true"));

		bPendingFreeMovementOnArrival = false;

		if (bHasExitTargetWorld)
		{
			UE_LOG(LogTemp, Warning, TEXT("[EXIT] Returning StartNavMesh command to %s"), *PendingExitTargetWorld.ToString());
			// Reached the border tile; now cross off the board via navmesh. Stay in ExitingBoard (input stays
			// disabled) until the player physically leaves the footprint — HandleBoardStateChanged then flips
			// to FreeMovement. Same rationale as the direct-exit case in ConfirmExitBoard.
			OwningController->SetMovementMode(EML_PlayerMovementMode::ExitingBoard);
			Cmd.bStartNavMesh   = true;
			Cmd.NavMeshTarget   = PendingExitTargetWorld;
			bHasExitTargetWorld = false;
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("[EXIT] ERROR: bHasExitTargetWorld is FALSE!"));
			OwningController->SetMovementMode(EML_PlayerMovementMode::FreeMovement);
		}

		PendingExitBorderTile = nullptr;
		return Cmd;
	}

	// Case 2: We arrived at the board entry tile via NavMesh — start tile-by-tile to actual target
	if (bPendingBoardEntryOnArrival)
	{
		bPendingBoardEntryOnArrival = false;
		OwningController->SetMovementMode(EML_PlayerMovementMode::InsideBoard);

		if (!IsValid(Character) || !IsValid(Character->CurrentTileOn)) return Cmd;

		AML_BoardSpawner* Board = Character->CurrentTileOn->GetBoardSpawnerFromTile();
		if (!IsValid(Board)) return Cmd;

		AML_Tile* TargetTile = PendingBoardEntryTargetTile;
		PendingBoardEntryTargetTile = nullptr;

		if (!IsValid(TargetTile) || TargetTile->GetOwner() != Board) return Cmd;

		const TMap<FIntPoint, AML_Tile*> GridMap = Board->GetGridMap();
		const FIntPoint StartAxial = Character->CurrentTileOn->GetAxialCoord();
		const FIntPoint GoalAxial  = TargetTile->GetAxialCoord();

		if (!GridMap.Contains(StartAxial) || !GridMap.Contains(GoalAxial)) return Cmd;
		if (!UML_HexPathfinder::IsTileWalkable(GridMap[StartAxial]) ||
			!UML_HexPathfinder::IsTileWalkable(GridMap[GoalAxial])) return Cmd;

		TArray<FIntPoint> AxialPath;
		if (!UML_HexPathfinder::BuildPath_AxialBFS(StartAxial, GoalAxial, GridMap, AxialPath)) return Cmd;

		BoardActionState = EML_PlayerBoardActionState::Moving;

		Cmd.bStartBoardPath = true;
		Cmd.BoardPath       = AxialPath;
		Cmd.BoardGridMap    = GridMap;
		return Cmd;
	}

	// Case 3: Arrived at tile adjacent to plant target — initiate turn
	if (BoardActionState == EML_PlayerBoardActionState::MovingToPlant)
	{
		if (!IsValid(PendingPlantTargetTile) || !IsValid(Character) || !IsValid(Character->CurrentTileOn))
		{
			PendingPlantTargetTile = nullptr;
			BoardActionState = EML_PlayerBoardActionState::Idle;
			return Cmd;
		}

		AML_BoardSpawner* Board = Character->CurrentTileOn->GetBoardSpawnerFromTile();
		if (!IsValid(Board))
		{
			PendingPlantTargetTile = nullptr;
			BoardActionState = EML_PlayerBoardActionState::Idle;
			return Cmd;
		}

		FML_TileNeighbors Neighbors;
		Board->GetNeighbors(Character->CurrentTileOn, Neighbors);
		if (Neighbors.Contains(PendingPlantTargetTile) &&
			UML_TileTypeTraits::CanPlayerPlant(PendingPlantTargetTile->GetCurrentType()) &&
			OwningController->HasEnergy())
		{
			StartTurnTowardTile(PendingPlantTargetTile);
			return Cmd;
		}

		PendingPlantTargetTile = nullptr;
		BoardActionState = EML_PlayerBoardActionState::Idle;
		return Cmd;
	}

	BoardActionState = EML_PlayerBoardActionState::Idle;
	return Cmd;
}
