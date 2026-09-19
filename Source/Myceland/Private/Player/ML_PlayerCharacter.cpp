// Copyright Myceland Team, All Rights Reserved.


#include "Player/ML_PlayerCharacter.h"

#include "Components/CapsuleComponent.h"
#include "EnhancedInputComponent.h"
#include "EngineUtils.h"
#include "FMODAudioComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Player/ML_HexPathfinder.h"
#include "Player/ML_PlayerController.h"
#include "Save System/ML_SaveSubsystem.h"
#include "Subsystem/ML_WinLoseSubsystem.h"
#include "TechArt/ML_NatureZone.h"
#include "Tiles/ML_Tile.h"
#include "Tiles/ML_TileBase.h"


AML_PlayerCharacter::AML_PlayerCharacter()
{
	PrimaryActorTick.bCanEverTick = true;
	
	GetCharacterMovement()->GravityScale = 1.5f;
	GetCharacterMovement()->MaxAcceleration = 1000.f;
	GetCharacterMovement()->BrakingFrictionFactor = 1.f;
	GetCharacterMovement()->BrakingDecelerationWalking = 1000.f;
	GetCharacterMovement()->PerchRadiusThreshold = 20.f;
	GetCharacterMovement()->bUseFlatBaseForFloorChecks = true;
	GetCharacterMovement()->RotationRate = FRotator(0.f, 640.f, 0.f);
	GetCharacterMovement()->bOrientRotationToMovement = true;
	GetCharacterMovement()->bConstrainToPlane = true;
	GetCharacterMovement()->bSnapToPlaneAtStart = true;
	
	AudioComponent = CreateDefaultSubobject<UFMODAudioComponent>(TEXT("FMODAudioComponent"));
	AudioComponent->SetupAttachment(RootComponent);
}

void AML_PlayerCharacter::UpdateCurrentTile()
{
	const FVector ActorLocation = GetActorLocation();

	float CapsuleRadius;
	float CapsuleHalfHeight;
	GetCapsuleComponent()->GetScaledCapsuleSize(CapsuleRadius, CapsuleHalfHeight);

	FVector Start = ActorLocation;
	FVector End = ActorLocation;
	End.Z = ActorLocation.Z - CapsuleHalfHeight - RaycastDistance;

	FHitResult Hit;
	FCollisionQueryParams Params;
	Params.AddIgnoredActor(this);
	Params.bTraceComplex = false;

	bool bHit = GetWorld()->LineTraceSingleByChannel(
		Hit,
		Start,
		End,
		ECC_Visibility,
		Params
	);
	
	AML_Tile* OldTile = CurrentTileOn;
	AML_Tile* NewTile = nullptr;

	if (bHit)
		if (AActor* HitActor = Hit.GetActor())
			if (AML_TileBase* TileBase = Cast<AML_TileBase>(HitActor))
				if (AActor* ParentActor = TileBase->GetAttachParentActor())
					NewTile = Cast<AML_Tile>(ParentActor);
			else
				NewTile = Cast<AML_Tile>(HitActor);

	if (NewTile == OldTile)
		return;

	CurrentTileOn = NewTile;
	OnCurrentTileChanged.Broadcast(OldTile, NewTile);
	HandleTileStateChange(OldTile, NewTile);
}

void AML_PlayerCharacter::HandleTileStateChange(const AML_Tile* OldTile, const AML_Tile* NewTile) const
{
	AML_BoardSpawner* OldBoard = OldTile ? OldTile->GetBoardSpawnerFromTile() : nullptr;
	AML_BoardSpawner* NewBoard = NewTile ? NewTile->GetBoardSpawnerFromTile() : nullptr;

	// No meaningful change.
	if (OldTile == NewTile)
		return;

	// Same board => internal movement, do not trigger camera / board change logic.
	if (OldBoard && NewBoard && OldBoard == NewBoard)
		return;

	// If we changed directly from one board to another, keep it.
	// If we genuinely crossed a gate, keep it too.
	OnBoardChanged.Broadcast(OldTile, NewTile);
}

void AML_PlayerCharacter::BeginPlay()
{
	Super::BeginPlay();
	MycelandController = Cast<AML_PlayerController>(GetController());

	// Defer by one tick: board BeginPlay calls (which restore solved grid state) all
	// happen during the same frame, so we wait until they have all finished before
	// querying which board's exit tile we should land on.
	GetWorld()->GetTimerManager().SetTimerForNextTick(this,
		&AML_PlayerCharacter::ApplySavedSpawnPosition);
}

void AML_PlayerCharacter::ApplySavedSpawnPosition()
{
	// Run the on-load replay at most once — this can be invoked more than once on load (e.g. pawn
	// re-possession), and a second run would restart the sequence mid-flight.
	if (bHasStartedOnLoadReplay) return;

	const UGameInstance* GI = GetGameInstance();
	if (!GI) return;

	const UML_SaveSubsystem* SaveSys = GI->GetSubsystem<UML_SaveSubsystem>();
	if (!SaveSys) return;

	const FName LastPuzzle = SaveSys->GetLastSolvedPuzzleID();
	if (LastPuzzle.IsNone()) return;

	UWorld* World = GetWorld();
	if (!World) return;

	// Build the replay order: every board the save marks solved, with the LATEST-solved board LAST so
	// the sequence walks the player from board to board and finally leaves them on it (as before).
	AML_BoardSpawner* LatestBoard = nullptr;
	TArray<AML_BoardSpawner*> OtherSolved;
	for (TActorIterator<AML_BoardSpawner> It(World); It; ++It)
	{
		AML_BoardSpawner* Board = *It;
		if (!IsValid(Board) || !Board->PuzzleID.IsValid()) continue;

		const FName PuzzleName = Board->PuzzleID.GetTagName();
		if (!SaveSys->IsPuzzleSolved(PuzzleName)) continue;

		if (PuzzleName == LastPuzzle)
			LatestBoard = Board;
		else
			OtherSolved.Add(Board);
	}

	OnLoadReplayQueue.Reset();
	for (AML_BoardSpawner* Board : OtherSolved)
		OnLoadReplayQueue.Add(Board);
	if (LatestBoard)
		OnLoadReplayQueue.Add(LatestBoard); // processed last → player ends here

	if (OnLoadReplayQueue.Num() == 0) return;

	bHasStartedOnLoadReplay = true;
	ProcessNextOnLoadBoard();
}

void AML_PlayerCharacter::ProcessNextOnLoadBoard()
{
	UWorld* World = GetWorld();
	if (!World) return;

	// Drop any boards that became invalid while queued.
	while (OnLoadReplayQueue.Num() > 0 && !OnLoadReplayQueue[0].IsValid())
		OnLoadReplayQueue.RemoveAt(0);

	if (OnLoadReplayQueue.Num() == 0) return; // done — player is on the latest-solved board

	AML_BoardSpawner* Board = OnLoadReplayQueue[0].Get();
	OnLoadReplayQueue.RemoveAt(0);

	// Teleport onto this board FIRST, then fire its OnWin, so listeners that read the player's current
	// tile / board (CurrentTileOn, CurrentBoardSpawner) produce the correct per-board result.
	if (TeleportToBoard(Board))
	{
		if (UML_WinLoseSubsystem* WinLose = World->GetSubsystem<UML_WinLoseSubsystem>())
			WinLose->ReplayOnWinForBoard(Board);

		// After OnWin, revitalize this board's nature zones (every solved board, latest included).
		for (AActor* ZoneActor : Board->GetAssociatedNatureZones())
		{
			if (AML_NatureZone* Zone = Cast<AML_NatureZone>(ZoneActor))
				Zone->Revive();
		}
	}

	if (OnLoadReplayQueue.Num() == 0) return; // that was the last board — stay here

	// Wait a few frames so this board's win reactions apply before teleporting to the next board.
	constexpr int32 FrameGap = 10;
	OnLoadReplayFramesLeft = FrameGap;
	World->GetTimerManager().SetTimerForNextTick(this, &AML_PlayerCharacter::TickOnLoadReplayGap);
}

void AML_PlayerCharacter::TickOnLoadReplayGap()
{
	UWorld* World = GetWorld();
	if (!World) return;

	if (--OnLoadReplayFramesLeft > 0)
	{
		World->GetTimerManager().SetTimerForNextTick(this, &AML_PlayerCharacter::TickOnLoadReplayGap);
		return;
	}

	ProcessNextOnLoadBoard();
}

bool AML_PlayerCharacter::TeleportToBoard(AML_BoardSpawner* Board)
{
	const AML_Tile* SpawnTile = FindWalkableSpawnTile(Board);
	if (!IsValid(SpawnTile))
	{
		UE_LOG(LogTemp, Warning, TEXT("[PlayerCharacter] Solved board '%s' has no walkable exit or water-path tile — cannot teleport for its OnWin replay."),
			Board ? *Board->PuzzleID.ToString() : TEXT("<null>"));
		return false;
	}

	// Place the player just above the tile so the character controller settles onto the surface.
	const float CapsuleHalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector SpawnLocation = SpawnTile->GetActorLocation() + FVector(0.f, 0.f, CapsuleHalfHeight + 10.f);
	TeleportTo(SpawnLocation, GetActorRotation());

	// Sync CurrentTileOn NOW (don't wait for the movement-based check in Tick) so the OnWin that
	// follows sees the player standing on this board.
	UpdateCurrentTile();
	return true;
}

const AML_Tile* AML_PlayerCharacter::FindWalkableSpawnTile(const AML_BoardSpawner* Board) const
{
	if (!IsValid(Board)) return nullptr;

	// 1. The first walkable ExitTile across all BoardExits (in order).
	for (const FML_BoardExit& Exit : Board->BoardExits)
		for (const TObjectPtr<AML_Tile>& ExitTile : Exit.ExitTiles)
			if (UML_HexPathfinder::IsTileWalkable(ExitTile.Get()))
				return ExitTile.Get();

	// 2. Failing that, the first walkable water-path tile (Exit then Entry, in order).
	for (const FML_WaterPath& WaterPath : Board->WaterPaths)
	{
		if (UML_HexPathfinder::IsTileWalkable(WaterPath.ExitTile.Get()))
			return WaterPath.ExitTile.Get();
		if (UML_HexPathfinder::IsTileWalkable(WaterPath.EntryTile.Get()))
			return WaterPath.EntryTile.Get();
	}

	return nullptr;
}

void AML_PlayerCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	
	// Only check if the character has moved
	FVector CurrentLocation = GetActorLocation();
	CurrentLocation.Z = 0.f;
	if (!LastCheckedLocation.Equals(CurrentLocation, 10.f)) // Tolerance of 10 units
	{
		UpdateCurrentTile();
		LastCheckedLocation = CurrentLocation;
	}
}

void AML_PlayerCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	// Demo cheat mode, and a no-op unless Enable Cheats is ticked in the Myceland Developer Settings.
	// Bound on the PAWN's input component rather than the controller's: the loading screen, the board
	// lock, the cinematics and the rollback all call DisableInput on the controller, which drops its
	// input component from the input stack. Bound there, the cheats would go dead in exactly the
	// situations they exist to get you out of - a cinematic to skip, a board that stays locked.
	if (AML_PlayerController* MycelandPlayerController = Cast<AML_PlayerController>(GetController()))
		MycelandPlayerController->BindCheatActions(Cast<UEnhancedInputComponent>(PlayerInputComponent));
}
