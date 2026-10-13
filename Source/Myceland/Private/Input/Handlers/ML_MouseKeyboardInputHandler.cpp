// Copyright Myceland Team, All Rights Reserved.

#include "Input/Handlers/ML_MouseKeyboardInputHandler.h"

#include "Player/ML_PlayerController.h"
#include "Player/ML_PlayerCharacter.h"
#include "Player/ML_HexPathfinder.h"
#include "Tiles/ML_Tile.h"
#include "Tiles/ML_BoardSpawner.h"

void UML_MouseKeyboardInputHandler::OnMoveActionStarted()
{
	bIgnoreCurrentClick = false;
	FollowTime = 0.f;
	Controller->ResetNavSteering();

	const EML_PlayerMovementMode Mode = Controller->GetMovementMode();
	if (Mode == EML_PlayerMovementMode::InsideBoard)
		HandleInsideBoardClick();
	else if (Mode == EML_PlayerMovementMode::FreeMovement)
		HandleFreeMovementClick();
}

void UML_MouseKeyboardInputHandler::OnMoveActionTriggered(float DeltaTime)
{
	FollowTime += DeltaTime;

	if (bIgnoreCurrentClick) return;

	if (Controller->GetMovementMode() != EML_PlayerMovementMode::FreeMovement)
		return;

	AML_PlayerCharacter* Character = Controller->GetMycelandCharacter();
	if (!IsValid(Character))
		return;

	FHitResult Hit;
	if (!Controller->GetHitResultUnderCursorByChannel(UEngineTypes::ConvertToTraceType(ECC_GameTraceChannel1), false, Hit))
		return;
	if (!Controller->IsClickableGround(Hit))
		return;

	// Keep driving the player toward the cursor even when it's over a non-walkable tile: the player
	// should approach the obstacle and stop just short of it, rather than freezing in place the moment
	// the cursor crosses onto an obstacle tile.
	HoldMoveCachedDestination = Hit.Location;

	// Steer along the navmesh rather than straight at the cursor, so holding walks AROUND obstacles
	// instead of grinding into them. A zero direction with a valid nav result means "path consumed,
	// stay put"; no nav result at all falls back to the raw direction so the hold never freezes.
	FVector Direction;
	if (!Controller->GetNavSteeringDirection(HoldMoveCachedDestination, DeltaTime, Direction))
		Direction = (HoldMoveCachedDestination - Character->GetActorLocation()).GetSafeNormal();

	if (!Direction.IsNearlyZero())
		Character->AddMovementInput(Direction, Controller->GetMoveSpeedScale());
}

void UML_MouseKeyboardInputHandler::OnMoveActionReleased()
{
	Controller->ResetNavSteering();

	const bool bWasHoldingExit = Controller->IsHoldingExitInput();
	Controller->CancelExitHold();
	if (bWasHoldingExit)
	{
		// Cursor is outside the board after releasing — clear the exit tile glow and path preview.
		Controller->ClearForcedHoverTile();
		Controller->ClearPathHoverPreview();
	}

	if (bIgnoreCurrentClick)
	{
		bIgnoreCurrentClick = false;
		FollowTime = 0.f;
		return;
	}

	if (Controller->GetMovementMode() == EML_PlayerMovementMode::FreeMovement)
	{
		if (FollowTime <= Controller->GetShortPressThreshold())
			Controller->StartNavMeshMovement(HoldMoveCachedDestination);
	}

	FollowTime = 0.f;
}

void UML_MouseKeyboardInputHandler::OnMoveAndPlantAction()
{
	AML_Tile* TargetTile = Controller->GetTileUnderCursor();
	Controller->Plant(TargetTile);
}

void UML_MouseKeyboardInputHandler::HandleInsideBoardClick()
{
	AML_PlayerCharacter* Character = Controller->GetMycelandCharacter();
	if (!IsValid(Character) || !IsValid(Character->CurrentTileOn)) return;

	AML_BoardSpawner* Board = Character->CurrentTileOn->GetBoardSpawnerFromTile();
	if (!IsValid(Board)) return;

	if (Controller->GetBoardActionState() == EML_PlayerBoardActionState::TurningToPlant) return;

	AML_Tile* TargetTile = Controller->GetTileUnderCursor();

	// Click on a tile in the board (move in board)
	if (IsValid(TargetTile) && TargetTile->GetOwner() == Board)
	{
		Controller->Move(TargetTile);
		return;
	}

	// If arrives here, then the click is outside the board (wants to leave the board).
	// The exit is only allowed on a designated ground surface (Ground channel) — not decor,
	// not arbitrary Cursor-blocking geometry.
	FHitResult Hit;
	if (!Controller->GetGroundUnderCursor(Hit)) return;

	AML_Tile* ExitGate = Controller->FindReachableExitBorderTile(Board, Hit.Location);
	if (!IsValid(ExitGate)) return;

	Controller->RequestExitHold(ExitGate, Hit.Location);
	// Show the cursor glow and path preview on the exit border tile while the hold is active.
	Controller->SetForcedHoverTile(ExitGate);
}

void UML_MouseKeyboardInputHandler::HandleFreeMovementClick()
{
	AML_Tile* TargetTile = Controller->GetTileUnderCursor();

	if (IsValid(TargetTile))
	{
		AML_BoardSpawner* Board = TargetTile->GetBoardSpawnerFromTile();
		if (!IsValid(Board)) return;

		if (!UML_HexPathfinder::IsTileWalkable(TargetTile))
		{
			bIgnoreCurrentClick = true;
			return;
		}

		Controller->StopNavMeshMovement();
		Controller->RequestBoardEntry(TargetTile);
		Controller->StartNavMeshMovement(TargetTile->GetActorLocation());
		return;
	}

	// No board tile hit: free movement on open ground.
	Controller->StopNavMeshMovement();
	FHitResult Hit;
	if (!Controller->GetHitResultUnderCursorByChannel(UEngineTypes::ConvertToTraceType(ECC_GameTraceChannel1), false, Hit)) return;
	if (!Controller->IsClickableGround(Hit)) return;
	HoldMoveCachedDestination = Hit.Location;
}
