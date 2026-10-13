// Copyright Myceland Team, All Rights Reserved.

#include "Input/Handlers/ML_GamepadInputHandler.h"

#include "Component/ML_BoardTransitionComponent.h"
#include "Core/ML_TileTypeTraits.h"
#include "Developer Settings/ML_MycelandDeveloperSettings.h"
#include "Player/ML_PlayerController.h"
#include "Player/ML_PlayerCharacter.h"
#include "Player/ML_HexPathfinder.h"
#include "Tiles/ML_Tile.h"
#include "Tiles/ML_BoardSpawner.h"
#include "Subsystem/ML_HexGridSubsystem.h"

// ==================== Lifecycle ====================

void UML_GamepadInputHandler::OnActivated()
{
	DevSettings = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();

	ResetMoveRepeatState();
	ResetSelectRepeatState();
	// All board visuals (plant selection, plantable highlight, exit-plane availability) are owned by the
	// HoverPreviewComponent and refreshed when the device switches to gamepad. The handler is input-only.
}

void UML_GamepadInputHandler::OnDeactivated()
{
	if (!Controller) return;

	if (Controller->IsHoldingExitInput())
		Controller->CancelExitHold();
	Controller->ClearForcedHoverTile();
	// The HoverPreviewComponent clears its board visuals (selection / plantable / exit) on the device switch.
}

// ==================== Left stick (movement) ====================

void UML_GamepadInputHandler::OnStickAxis(FVector2D StickValue, float DeltaTime)
{
	if (!Controller) return;

	const EML_PlayerMovementMode Mode = Controller->GetMovementMode();

	// On a world board the player always moves tile by tile, like inside a puzzle (same step cadence).
	// Elsewhere in free movement (open ground, a solved puzzle in a level without world boards) the legacy
	// analog movement stays until those levels are converted (plan: phase 9).
	const AML_PlayerCharacter* Character = Controller->GetMycelandCharacter();
	const AML_BoardSpawner* CurrentBoard = IsValid(Character) && IsValid(Character->CurrentTileOn)
		? Character->CurrentTileOn->GetBoardSpawnerFromTile() : nullptr;
	const bool bOnWorldBoard = IsValid(CurrentBoard) && CurrentBoard->IsWorldBoard();

	if (Mode == EML_PlayerMovementMode::FreeMovement && bOnWorldBoard)
		HandleInsideBoardMoveStick(StickValue, DeltaTime);
	else if (Mode == EML_PlayerMovementMode::FreeMovement)
		HandleFreeMovementStick(StickValue);
	else if (Mode == EML_PlayerMovementMode::InsideBoard)
		HandleInsideBoardMoveStick(StickValue, DeltaTime);
	else if (Mode == EML_PlayerMovementMode::ExitingBoard)
		HandleExitingBoardStick(StickValue);
}

void UML_GamepadInputHandler::OnStickReleased()
{
	// Releasing the stick during an exit hold cancels it (mouse-parity: releasing the button cancels).
	if (Controller && Controller->IsHoldingExitInput())
	{
		Controller->CancelExitHold();
		Controller->ClearForcedHoverTile();
	}

	ResetMoveRepeatState();
}

void UML_GamepadInputHandler::HandleFreeMovementStick(FVector2D StickValue)
{
	if (StickValue.IsNearlyZero()) return;

	AML_PlayerCharacter* Character = Controller->GetMycelandCharacter();
	if (!IsValid(Character)) return;

	// Cancel any ongoing navmesh movement so the stick has immediate full control.
	Controller->StopNavMeshMovement();

	const FVector WorldDirection = StickToWorldDirection(StickValue);
	Character->AddMovementInput(WorldDirection, FreeMovementScale);
}

void UML_GamepadInputHandler::HandleInsideBoardMoveStick(FVector2D StickValue, float DeltaTime)
{
	// No movement while the character is turning to plant (no input accepted in that state).
	if (Controller->GetBoardActionState() == EML_PlayerBoardActionState::TurningToPlant) return;

	const float HoldDelay = DevSettings ? DevSettings->GamepadMoveHoldRepeatDelay : 0.3f;
	const float RepeatInterval = DevSettings ? FMath::Max(DevSettings->GamepadMoveHoldRepeatInterval, 0.01f) : 0.1f;

	// First frame after returning from neutral: step immediately, then start the hold timer.
	if (bMoveStickWasNeutral)
	{
		bMoveStickWasNeutral = false;
		StepMoveInStickDirection(StickValue);
		return;
	}

	// Stick held: wait out the initial delay before the player starts auto-stepping to the next tile.
	if (!bMoveAutoRepeatActive)
	{
		MoveStickHeldTime += DeltaTime;
		if (MoveStickHeldTime < HoldDelay) return;

		bMoveAutoRepeatActive = true;
		StepMoveInStickDirection(StickValue);
		return;
	}

	// Auto-step active: step to the next tile once per RepeatInterval while held.
	MoveRepeatTimer += DeltaTime;
	while (MoveRepeatTimer >= RepeatInterval)
	{
		MoveRepeatTimer -= RepeatInterval;
		StepMoveInStickDirection(StickValue);
	}
}

void UML_GamepadInputHandler::StepMoveInStickDirection(FVector2D StickValue)
{
	AML_PlayerCharacter* Character = Controller->GetMycelandCharacter();
	if (!IsValid(Character) || !IsValid(Character->CurrentTileOn)) return;

	AML_BoardSpawner* Board = Character->CurrentTileOn->GetBoardSpawnerFromTile();
	if (!IsValid(Board)) return;

	const float Threshold = FMath::Max(DevSettings ? DevSettings->GamepadMoveAlignmentThreshold : 0.5f, 0.f);

	// Best walkable neighbor aligned with the stick (NeighborDot measures how well it matches).
	float NeighborDot = -1.f;
	AML_Tile* Neighbor = FindNeighborInStickDirection(StickValue, Threshold, false, 1, &NeighborDot);

	// On an exit border tile, pushing the stick toward the exit plane starts leaving the board — there is
	// no dedicated exit button anymore. The exit direction competes with the neighbor tiles: it only wins
	// when the stick points at the plane at least as well as at any walkable neighbor.
	float ExitDot = -1.f;
	FVector ExitTarget = FVector::ZeroVector;
	if (EvaluateStickExit(StickValue, ExitDot, ExitTarget) && ExitDot >= Threshold && ExitDot >= NeighborDot)
	{
		// Reuse the shared exit-hold flow (same as the mouse). With ExitBoardHoldDurationGamepad = 0 it resolves
		// on the first tick (instant); with a positive duration the player must keep the stick pointed at
		// the exit — HandleExitingBoardStick keeps the hold alive, releasing/steering away cancels it.
		Controller->RequestExitHold(Character->CurrentTileOn, ExitTarget);
		Controller->SetForcedHoverTile(Character->CurrentTileOn); // Exit-tile glow while the hold is active.
		return;
	}

	if (IsValid(Neighbor))
		Controller->Move(Neighbor);
}

void UML_GamepadInputHandler::ResetMoveRepeatState()
{
	bMoveStickWasNeutral = true;
	MoveStickHeldTime = 0.f;
	MoveRepeatTimer = 0.f;
	bMoveAutoRepeatActive = false;
}

void UML_GamepadInputHandler::ResetSelectRepeatState()
{
	bSelectStickWasNeutral = true;
	SelectStickHeldTime = 0.f;
	SelectRepeatTimer = 0.f;
	bSelectAutoRepeatActive = false;
}

// ==================== Right stick (plant selection) ====================

void UML_GamepadInputHandler::OnPlantSelectAxis(FVector2D StickValue, float DeltaTime)
{
	if (!Controller || Controller->GetMovementMode() != EML_PlayerMovementMode::InsideBoard) return;
	if (Controller->GetBoardActionState() == EML_PlayerBoardActionState::TurningToPlant) return;

	// Stick back in the neutral zone: re-arm the next push to select immediately (flick) and drop the hold
	// state. The current selection is left untouched (it persists like the mouse cursor), so re-holding just
	// resumes from where the selection already was.
	if (StickValue.IsNearlyZero())
	{
		ResetSelectRepeatState();
		return;
	}

	// Right stick mirrors the left stick's move cadence, but with its own tuning: a flick selects once
	// immediately, then the stick must be held GamepadSelectHoldRepeatDelay before the selection starts
	// auto-advancing, one step per GamepadSelectHoldRepeatInterval while held.
	const float HoldDelay = DevSettings ? DevSettings->GamepadSelectHoldRepeatDelay : 0.3f;
	const float RepeatInterval = DevSettings ? FMath::Max(DevSettings->GamepadSelectHoldRepeatInterval, 0.01f) : 0.1f;

	// Flick (first frame away from neutral): select once immediately, then start the hold timer.
	if (bSelectStickWasNeutral)
	{
		bSelectStickWasNeutral = false;
		UpdatePlantSelection(StickValue);
		return;
	}

	// Stick held: wait out the initial delay before the selection starts auto-advancing to the next tile.
	if (!bSelectAutoRepeatActive)
	{
		SelectStickHeldTime += DeltaTime;
		if (SelectStickHeldTime < HoldDelay) return;

		bSelectAutoRepeatActive = true;
		UpdatePlantSelection(StickValue);
		return;
	}

	// Auto-advance active: step the selection once per RepeatInterval while held.
	SelectRepeatTimer += DeltaTime;
	while (SelectRepeatTimer >= RepeatInterval)
	{
		SelectRepeatTimer -= RepeatInterval;
		UpdatePlantSelection(StickValue);
	}
}

void UML_GamepadInputHandler::OnPlantSelectReleased()
{
	// Selection itself is persistent (mirrors the mouse cursor staying on the last hovered tile), so we
	// keep the selected tile. We only reset the hold-repeat state so the next push selects immediately —
	// covers input backends that stop broadcasting axis events at neutral instead of sending a zero value.
	ResetSelectRepeatState();
}

void UML_GamepadInputHandler::UpdatePlantSelection(FVector2D StickValue)
{
	if (StickValue.IsNearlyZero()) return;

	AML_PlayerCharacter* Character = Controller->GetMycelandCharacter();
	if (!IsValid(Character) || !IsValid(Character->CurrentTileOn)) return;

	AML_BoardSpawner* Board = Character->CurrentTileOn->GetBoardSpawnerFromTile();
	if (!IsValid(Board)) return;

	const float Threshold = FMath::Max(DevSettings ? DevSettings->GamepadSelectAlignmentThreshold : 0.5f, 0.f);
	const int32 MaxRing = DevSettings ? FMath::Max<int32>(DevSettings->GamepadSelectRingDistance, 1) : 1;

	// The cursor steps one tile at a time, relative to the tile that is currently selected — flicking a
	// direction moves to that tile's neighbor in the stick direction (not the best tile around the player).
	// When nothing is selected yet, the first push starts from the player's tile (so it steps onto a ring-1
	// neighbor). The step can land on ANY tile that stays within MaxRing of the player (plantable or not) so
	// the cursor can hover the whole board around the player like the mouse — the plant button (OnMoveAndPlantAction
	// -> Plant()) rejects non-plantable tiles, so hovering one is harmless.
	AML_Tile* OriginTile = Controller->GetGamepadSelectedTile();
	if (!IsValid(OriginTile)) OriginTile = Character->CurrentTileOn;

	FML_TileNeighbors Neighbors;
	Board->GetNeighbors(OriginTile, Neighbors);

	const FVector StickWorldDir = StickToWorldDirection(StickValue);
	const FVector OriginPos = OriginTile->GetActorLocation();
	const FIntPoint PlayerAxial = Character->CurrentTileOn->GetAxialCoord();

	AML_Tile* Best = nullptr;
	float BestDot = -1.f;
	for (AML_Tile* Neighbor : Neighbors)
	{
		if (!IsValid(Neighbor)) continue;
		// Non-interactable tiles (the obstacle ring framing the board) are dropped from the candidate set,
		// mirroring the mouse, which cannot hover them either. This is a one-tile step, not a search: the
		// cursor does NOT jump over a rejected tile to a farther one — it lands on the best remaining
		// neighbor within the alignment threshold, or stays where it is.
		if (!Neighbor->IsInteractable()) continue;
		if (HexAxialDistance(Neighbor->GetAxialCoord(), PlayerAxial) > MaxRing) continue;

		const FVector ToNeighbor = (Neighbor->GetActorLocation() - OriginPos).GetSafeNormal2D();
		const float Dot = FVector::DotProduct(StickWorldDir, ToNeighbor);
		if (Dot > BestDot)
		{
			BestDot = Dot;
			Best = Neighbor;
		}
	}

	if (!IsValid(Best) || BestDot < Threshold) return;
	if (Best == Controller->GetGamepadSelectedTile()) return;

	// The HoverPreviewComponent owns the selection state + its cursor glow.
	Controller->SetGamepadSelectedTile(Best);
}

void UML_GamepadInputHandler::OnMoveAndPlantAction()
{
	if (!Controller) return;
	Controller->Plant(Controller->GetGamepadSelectedTile()); // Plant() rejects gracefully if null
}

// ==================== Exit ====================

void UML_GamepadInputHandler::HandleExitingBoardStick(FVector2D StickValue)
{
	if (!Controller->IsHoldingExitInput()) return;

	// Mouse-parity hold: the exit only completes if the player keeps the stick pointed at the exit for
	// the full ExitBoardHoldDurationGamepad. Steering the stick away from the exit cancels the hold; a full
	// release is handled by OnStickReleased. (With ExitBoardHoldDurationGamepad = 0 the hold resolves before
	// this ever runs, so this path only matters for a positive hold duration.)
	const float Threshold = FMath::Max(DevSettings ? DevSettings->GamepadMoveAlignmentThreshold : 0.5f, 0.f);

	float ExitDot = -1.f;
	FVector ExitTarget = FVector::ZeroVector;
	if (!EvaluateStickExit(StickValue, ExitDot, ExitTarget) || ExitDot < Threshold)
	{
		Controller->CancelExitHold();
		Controller->ClearForcedHoverTile();
	}
}

// ==================== Helpers ====================

bool UML_GamepadInputHandler::EvaluateStickExit(FVector2D StickValue, float& OutExitDot, FVector& OutExitTarget) const
{
	AML_PlayerCharacter* Character = Controller ? Controller->GetMycelandCharacter() : nullptr;
	if (!IsValid(Character) || !IsValid(Character->CurrentTileOn)) return false;

	AML_BoardSpawner* Board = Character->CurrentTileOn->GetBoardSpawnerFromTile();
	if (!IsValid(Board)) return false;

	// GetActiveExitPlaneForTile returns null for a missing OR disabled exit, so the stick can't walk out
	// through a ground exit the player couldn't otherwise take (same rule as the mouse and the highlight).
	AActor* ExitPlane = Board->GetActiveExitPlaneForTile(Character->CurrentTileOn);
	if (!IsValid(ExitPlane)) return false;

	OutExitTarget = ExitPlane->GetActorLocation();
	const FVector StickWorldDir = StickToWorldDirection(StickValue);
	const FVector ToExit = (OutExitTarget - Character->CurrentTileOn->GetActorLocation()).GetSafeNormal2D();
	OutExitDot = FVector::DotProduct(StickWorldDir, ToExit);
	return true;
}

FVector UML_GamepadInputHandler::StickToWorldDirection(FVector2D StickValue) const
{
	if (!Controller) return FVector::ForwardVector;

	// Use the camera's actual yaw so movement is relative to what the player sees.
	// GetControlRotation() is not updated when the view target is a camera rail.
	const float CameraYaw = Controller->PlayerCameraManager
		? Controller->PlayerCameraManager->GetCameraRotation().Yaw
		: Controller->GetControlRotation().Yaw;

	const FRotator YawRotation(0.f, CameraYaw, 0.f);
	const FVector WorldForward = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
	const FVector WorldRight   = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

	return (WorldForward * StickValue.Y + WorldRight * StickValue.X).GetSafeNormal();
}

int32 UML_GamepadInputHandler::HexAxialDistance(const FIntPoint& A, const FIntPoint& B)
{
	// Axial -> cube distance: (|dq| + |dq+dr| + |dr|) / 2.
	const int32 dq = A.X - B.X;
	const int32 dr = A.Y - B.Y;
	return (FMath::Abs(dq) + FMath::Abs(dq + dr) + FMath::Abs(dr)) / 2;
}

AML_Tile* UML_GamepadInputHandler::FindNeighborInStickDirection(FVector2D StickValue, float AlignmentThreshold, bool bPlantableOnly, int32 RingDistance, float* OutBestDot) const
{
	if (OutBestDot) *OutBestDot = -1.f;

	AML_PlayerCharacter* Character = Controller ? Controller->GetMycelandCharacter() : nullptr;
	if (!IsValid(Character) || !IsValid(Character->CurrentTileOn)) return nullptr;

	AML_BoardSpawner* Board = Character->CurrentTileOn->GetBoardSpawnerFromTile();
	if (!IsValid(Board)) return nullptr;

	AML_Tile* OriginTile = Character->CurrentTileOn;
	RingDistance = FMath::Max(RingDistance, 1);

	// Movement steps go through the global grid: neighbors may belong to another board (stepping out of a
	// puzzle onto the world, across world boards), and the step is measured from the tile the player is
	// walking toward, so holding the stick chains tiles without turning back.
	const UML_HexGridSubsystem* HexGrid = UML_HexGridSubsystem::Get(Controller);
	const bool bGridStep = !bPlantableOnly && RingDistance == 1 && HexGrid != nullptr;
	if (bGridStep)
	{
		if (AML_Tile* MoveOrigin = Controller->GetMoveOriginTile())
			OriginTile = MoveOrigin;
	}

	// Gather every candidate tile within RingDistance of the player (hex distance 1..RingDistance,
	// origin excluded). Ring 1 uses the fast neighbor lookup; larger ranges scan the grid map.
	TArray<AML_Tile*, TInlineAllocator<24>> Candidates;
	if (bGridStep)
	{
		FML_TileNeighbors Neighbors;
		HexGrid->GetNeighbors(OriginTile, Neighbors);
		for (AML_Tile* Neighbor : Neighbors)
			if (IsValid(Neighbor)) Candidates.Add(Neighbor);
	}
	else if (RingDistance == 1)
	{
		FML_TileNeighbors Neighbors;
		Board->GetNeighbors(OriginTile, Neighbors);
		for (AML_Tile* Neighbor : Neighbors)
			if (IsValid(Neighbor)) Candidates.Add(Neighbor);
	}
	else
	{
		const FIntPoint OriginAxial = OriginTile->GetAxialCoord();
		for (const TPair<FIntPoint, AML_Tile*>& Pair : Board->GetGridMapRef())
		{
			if (!IsValid(Pair.Value)) continue;
			const int32 Dist = HexAxialDistance(Pair.Key, OriginAxial);
			if (Dist >= 1 && Dist <= RingDistance)
				Candidates.Add(Pair.Value);
		}
	}
	if (Candidates.IsEmpty()) return nullptr;

	const FVector StickWorldDir = StickToWorldDirection(StickValue);
	const FVector OriginPos = OriginTile->GetActorLocation();

	AML_Tile* BestNeighbor = nullptr;
	float BestDot = -1.f;

	for (AML_Tile* Neighbor : Candidates)
	{
		// Movement only walks onto walkable tiles; plant selection only picks plantable tiles.
		// IsTileWalkable already rejects non-interactable tiles, so only the plantable branch needs the
		// explicit check (a Dirt tile flagged non-interactable must not be offered either).
		// A grid step uses the full step rule (walkable + height), so the stick never offers a step the
		// pathfinder would refuse.
		const bool bCandidate = bPlantableOnly
			? (Neighbor->IsInteractable() && UML_TileTypeTraits::CanPlayerPlant(Neighbor->GetCurrentType()))
			: (bGridStep ? UML_HexPathfinder::CanStep(OriginTile, Neighbor) : UML_HexPathfinder::IsTileWalkable(Neighbor));
		if (!bCandidate) continue;

		const FVector ToNeighbor = (Neighbor->GetActorLocation() - OriginPos).GetSafeNormal2D();
		const float Dot = FVector::DotProduct(StickWorldDir, ToNeighbor);

		if (Dot > BestDot)
		{
			BestDot = Dot;
			BestNeighbor = Neighbor;
		}
	}

	const bool bClears = (BestDot >= AlignmentThreshold);
	if (OutBestDot) *OutBestDot = bClears ? BestDot : -1.f;
	return bClears ? BestNeighbor : nullptr;
}
