// Copyright Myceland Team, All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Interfaces/ML_DialogueSpeaker.h"
#include "Myceland/Public/Tiles/ML_BoardSpawner.h"
#include "ML_PlayerCharacter.generated.h"

class AML_PlayerController;
class UCameraComponent;
class USpringArmComponent;
class AML_Tile;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnCurrentTileChanged, const AML_Tile*, OldTile, const AML_Tile*, NewTile);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnBoardChanged, const AML_Tile*, OldTile, const AML_Tile*, NewTile);

UCLASS()
class MYCELAND_API AML_PlayerCharacter : public ACharacter, public IML_DialogueSpeaker
{
	GENERATED_BODY()
	
private:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	UFMODAudioComponent* AudioComponent;

	// Reads LastSolvedPuzzleID from the save and teleports the player to that puzzle's exit tile.
	// Called one tick after BeginPlay so all boards have finished restoring their grids.
	void ApplySavedSpawnPosition();
    	
	UPROPERTY()
	AML_PlayerController* MycelandController;
	
	FVector LastCheckedLocation = FVector::ZeroVector;
	bool bIsTalking = false;
	const float RaycastDistance = 50.f;
	
	void HandleTileStateChange(const AML_Tile* OldTile, const AML_Tile* NewTile) const;

	// ---- On-load win replay ----
	// On load, for every board the save marks solved we teleport the player onto it, fire its OnWin,
	// wait a couple frames, then move to the next — ending on the latest-solved board. Teleporting
	// first means each board's win effects run with the player actually standing on it.
	TArray<TWeakObjectPtr<AML_BoardSpawner>> OnLoadReplayQueue;
	int32 OnLoadReplayFramesLeft = 0;
	bool bHasStartedOnLoadReplay = false;

	// Teleports onto the next queued board, fires its OnWin, then waits a couple frames and repeats.
	void ProcessNextOnLoadBoard();

	// Frame-gap countdown between boards (chained next-tick timers); calls ProcessNextOnLoadBoard at 0.
	void TickOnLoadReplayGap();

	// Teleports the player onto a walkable tile of Board and syncs CurrentTileOn. False if none walkable.
	bool TeleportToBoard(AML_BoardSpawner* Board);

	// First walkable exit tile (across BoardExits), else first walkable water-path tile, else nullptr.
	const AML_Tile* FindWalkableSpawnTile(const AML_BoardSpawner* Board) const;

public:
	virtual void BeginPlay() override;
	
	void UpdateCurrentTile();

	UPROPERTY(BlueprintReadOnly, Category="Myceland Character")
	AML_Tile* CurrentTileOn = nullptr;
	
	UPROPERTY(BlueprintAssignable, Category="Myceland Character|Delegates")
	FOnBoardChanged OnBoardChanged;
	
	UPROPERTY(BlueprintAssignable, Category = "Myceland Character|Delegates")
	FOnCurrentTileChanged OnCurrentTileChanged;
	
	AML_PlayerCharacter();
	virtual void Tick(float DeltaTime) override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
	
	// ~Begin IML_DialogueSpeaker Implementation
	virtual void SetIsTalking(const bool bTalking) override { this->bIsTalking = bTalking; }
	
	UFUNCTION(BlueprintCallable, Category="Dialogue")
    virtual bool IsTalking() const override { return bIsTalking; }
	
	UFUNCTION(BlueprintCallable, Category="Dialogue")
	virtual UFMODAudioComponent* GetAudioComponent() const override { return AudioComponent; }
	// ~End IML_DialogueSpeaker Implementation
};
