// Copyright Myceland Team, All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ML_CoreData.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "Save System/ML_GameSaveData.h"
#include "PuzzleGeneration/ML_PuzzleGenerationTypes.h"
#include "LevelSequence.h"
#include "CineCameraActor.h"
#include "Actors/ML_CinematicCameraRail.h"
#include "ML_BoardSpawner.generated.h"

class UML_BiomeTileSet;
class UML_SaveSubsystem;
class AML_CameraRail;
class AML_Collectible;
class AML_TileBase;
class AML_TileWater;
class AML_TileParasite;
class AML_TileGrass;
class AML_Tile;

// Fixed-capacity neighbor list (6 hex directions) stored inline on the stack:
// filling it never allocates heap memory, unlike a regular TArray.
using FML_TileNeighbors = TArray<AML_Tile*, TInlineAllocator<6>>;

// What the prerequisite rule must do to a board's switches this pass (see RefreshLockState).
// Release exists so the rule can forget a board it had locked without switching anything back on:
// a board that got solved while locked must stay off, exactly like any other solved board.
enum class EML_BoardLockAction : uint8
{
	None,
	Lock,
	Unlock,
	Release
};

// All categories below are prefixed "ML- " so typing "ML" in the Details panel search box
// filters the panel down to this class's own categories. The Details panel sorts categories by
// declaration order, never by name, so the prefix is for searching only — don't expect it to sort.
UCLASS()
class MYCELAND_API AML_BoardSpawner : public AActor
{
	GENERATED_BODY()

private:
	// ==================== ML- Runtime ====================

	UPROPERTY(EditInstanceOnly, Category="ML- Runtime")
	UML_BiomeTileSet* BiomeTileSet;

	UPROPERTY(EditInstanceOnly, Category="ML- Runtime")
	ACameraActor* AssociatedCamera;

	UPROPERTY(EditInstanceOnly, Category="ML- Runtime")
	AActor* AssociatedObstacle;

	UPROPERTY(EditInstanceOnly, Category="ML- Runtime")
	TArray<AActor*> AssociatedNatureZones;

	UPROPERTY(EditInstanceOnly, Category="ML- Runtime")
	TArray<AActor*> AssociatedWaterPaths;

	UPROPERTY(EditInstanceOnly, Category="ML- Runtime")
	ULevelSequence* AssociatedWinCinematic;

	UPROPERTY(EditInstanceOnly, Category="ML- Runtime")
	TObjectPtr<AML_CinematicCameraRail> AssociatedStartCinematicRail = nullptr;

	UPROPERTY(Transient)
	TArray<TObjectPtr<AML_Tile>> SpawnedTiles;

	UPROPERTY(Transient)
	TArray<TObjectPtr<AML_Tile>> TreeTiles;

	TMap<FIntPoint, TObjectPtr<AML_Tile>> GridMap;

	mutable TMap<FIntPoint, AML_Tile*> GridMapCache;
	mutable bool bGridMapCacheBuilt = false;

	// ========== Board switches (per-instance, runtime-toggleable) ==========
	// Kept private so all runtime changes go through the setters (SetGlowEnabled clears any
	// active glow on OFF). EditAnywhere/BlueprintReadOnly still expose them via reflection.

	// When false, this board's tiles never glow (cursor hover + path preview).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="ML- Board Switches", meta=(AllowPrivateAccess="true"))
	bool bGlowEnabled = true;

	// When false, the player never enters this board's tile-by-tile mode: clicking it stays in
	// free movement, and there is no exit-hold. The board movement system is fully disabled for it.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="ML- Board Switches", meta=(AllowPrivateAccess="true"))
	bool bBoardTransitionEnabled = true;

	// World board: the terrain the player walks on between puzzles, not a puzzle. Walking onto it never stops
	// the player. Movement itself treats every board the same; this only says what the board IS.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="ML- Board Switches", meta=(AllowPrivateAccess="true"))
	bool bIsWorldBoard = false;

	// Boards that must be solved before this one opens. Leave empty (the default) and the board is
	// never locked — it keeps whatever its switches are set to, which is what every existing board
	// and the hub rely on. Fill it and the board starts glow-less and non-enterable until all of
	// them are solved, so a puzzle the player has not reached yet stops glowing under the cursor.
	// Solved state is read from the save, so the order boards run BeginPlay in does not matter.
	UPROPERTY(EditInstanceOnly, Category="ML- Board Switches", meta=(AllowPrivateAccess="true", DisplayName="Required Puzzles"))
	TArray<TObjectPtr<AML_BoardSpawner>> RequiredPuzzles;

	// True while THIS rule is what turned the switches off, so unlocking only ever gives back what
	// the rule took: a board switched off by a designer, by a win, or by Blueprint is never
	// re-enabled behind their back.
	bool bLockedByPrerequisites = false;
	// Sequential activation of win effect tiles.
	FTimerHandle WinEffectTimerHandle;

	void ActivateNextWinEffectTile(int32 Index);
	// Generators
	void UpdateCurrentGrid(bool bAllowSpawn = true);
	void SpawnHexagonRadius();
	void SpawnRectangleWH();
	void RefreshTileCaches();

	// Conversions
	FVector AxialToWorld(int32 Q, int32 R) const;
	FIntPoint OffsetToAxial(int32 Col, int32 Row) const;

	// Warns when this board is off the global lattice or has tiles between two height levels: either one
	// would break walking between boards. Each warning names the button that fixes it. No-op in shipping.
	void ValidateGridAlignment() const;

	FML_PuzzleState BuildPuzzleStateFromCurrentGrid() const;

	// Blueprint-only version (UHT can't expose the inline-allocator array above, so this
	// one copies into a regular TArray). Private on purpose: Blueprint ignores C++ access
	// specifiers so BP graphs can still call it, while C++ code is forced to use the
	// allocation-free overload above.
	UFUNCTION(BlueprintCallable, Category="ML- Hex Grid")
	TArray<AML_Tile*> GetNeighbors(AML_Tile* CenterTile) const;

protected:
	virtual void Destroyed() override;
	virtual void BeginPlay() override;

	// Leaves the global hex grid (UML_HexGridSubsystem), so a streamed-out board takes its tiles with it.
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

#if WITH_EDITOR
	// Editing bGlowEnabled/bBoardTransitionEnabled in the Details panel writes the field directly,
	// bypassing the setters. Re-run their side-effects here so editor toggles behave like the setters.
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

public:
	AML_BoardSpawner();
	FIntPoint WorldToAxial(const FVector& WorldLocation) const;

	// Lattice math on the global hex grid (tile size + orientation from the developer settings).
	// "Local" is a 2D offset from a lattice point (a board origin, or the global Hex Grid Origin).
	static FVector2D AxialToLocal2D(int32 Q, int32 R);
	static FIntPoint Local2DToAxial(const FVector2D& Local);

	// Closest point of the global lattice to WorldLocation (XY snapped, Z kept).
	static FVector SnapToLattice(const FVector& WorldLocation);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Myceland Runtime")
	int32 EnergyForPuzzle = 1;

	// ==================== ML- Hex Grid ====================

	UPROPERTY(EditAnywhere, Category="ML- Hex Grid")
	TSubclassOf<AML_Tile> TileClass;

	UPROPERTY(EditAnywhere, Category="ML- Hex Grid")
	EML_HexGridLayout GridLayout = EML_HexGridLayout::HexagonRadius;

	UPROPERTY(EditAnywhere, Category="ML- Hex Grid", meta=(ClampMin="0", EditCondition="GridLayout==EML_HexGridLayout::HexagonRadius", EditConditionHides))
	int32 Radius = 2;

	UPROPERTY(EditAnywhere, Category="ML- Hex Grid|Rectangle", meta=(ClampMin="1", EditCondition="GridLayout==EML_HexGridLayout::RectangleWH", EditConditionHides))
	int32 GridWidth = 5;

	UPROPERTY(EditAnywhere, Category="ML- Hex Grid|Rectangle", meta=(ClampMin="1", EditCondition="GridLayout==EML_HexGridLayout::RectangleWH", EditConditionHides))
	int32 GridHeight = 5;

	UPROPERTY(EditAnywhere, Category="ML- Hex Grid", meta=(EditCondition="GridLayout==EML_HexGridLayout::RectangleWH", EditConditionHides))
	EML_HexOffsetLayout OffsetLayout = EML_HexOffsetLayout::OddR;

	UPROPERTY(EditAnywhere, Category="ML- Hex Grid")
	FRotator TileRotation = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, Category="ML- Hex Grid", meta=(ClampMin="0.01"))
	FVector TileScale = FVector(2.f, 2.f, 2.f);
	

	UPROPERTY(EditAnywhere, Category="ML- Hex Grid", meta=(Categories="Puzzle"))
	FGameplayTag PuzzleID;

	// Actors that should receive Set Tile Alive when this puzzle is won.
	// Assign the desired tile actors per board instance in the Details panel.
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category="ML- Win Effects", meta=(DisplayName="Win Effect Tiles"))
	TArray<TObjectPtr<AML_Tile>> WinEffectTiles;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="ML- Win Effects", meta=(DisplayName="Win Effect Tile Delay", ClampMin="0.0", UIMin="0.0", Units="Seconds"))
	float WinEffectTileDelay = 0.2f;

	UPROPERTY(EditAnywhere, Category="ML- Hex Grid")
	TArray<AML_CameraRail*> AssociatedCameraRails;


	UPROPERTY(EditInstanceOnly, Category="ML- Runtime")
	TArray<AActor*> ExitGrounds;
	
	UPROPERTY(EditAnywhere, Category="ML- Hex Grid")
	TArray<FML_WaterPath> WaterPaths;

	UPROPERTY(EditAnywhere, Category="ML- Hex Grid")
	TSubclassOf<AML_TileBase> WaterChangeTile;

  UPROPERTY(VisibleInstanceOnly, BlueprintReadWrite, Category="ML- Debug", Transient)
    bool bIsPuzzleSolved = false;

	UFUNCTION(CallInEditor, Category="ML- Hex Grid", meta=(DisplayName="Update Current Grid"))
	void UpdateCurrentGridEditor();

	UFUNCTION(CallInEditor, Category="ML- Hex Grid", meta=(DisplayName="Clear & Rebuild Grid"))
	void RebuildGrid();

	UFUNCTION(CallInEditor, Category="ML- Hex Grid", meta=(DisplayName="Clear Grid"))
	void ClearTiles();

	UFUNCTION(CallInEditor, Category="ML- Hex Grid", meta=(DisplayName="Destroy Stale Tiles"))
	void DestroyStaleTiles() const;

	// Tile size and orientation now come from Project Settings > Myceland > Gameplay > Hex Grid, shared by
	// every board so the player can walk from one board to another.

	// Moves this board (and its attached tiles) onto the closest point of the global hex lattice. Only XY
	// moves, never Z. Needed for the board's tiles to line up with the neighboring boards' tiles.
	UFUNCTION(CallInEditor, Category="ML- Hex Grid", meta=(DisplayName="Snap Board To Lattice"))
	void SnapBoardToLattice();

	// Moves every tile of this board to the exact Z of its closest height level, then refreshes the levels.
	// Workflow: raise or lower tiles by hand, then click this.
	UFUNCTION(CallInEditor, Category="ML- Hex Grid", meta=(DisplayName="Snap Tile Heights"))
	void SnapTileHeights();

	// Fills the caller-provided inline buffer (no heap allocation, reusable across calls).
	// Always outputs 6 entries, nullptr where no neighbor exists.
	void GetNeighbors(const AML_Tile* CenterTile, FML_TileNeighbors& OutNeighbors) const;

	UFUNCTION(BlueprintPure, Category="ML- Runtime")
	TMap<FIntPoint, AML_Tile*> GetGridMap() const;

	const TMap<FIntPoint, AML_Tile*>& GetGridMapRef() const;

	UFUNCTION(BlueprintPure, Category="ML- Runtime")
	TArray<AML_Tile*> GetGridTiles();

	UFUNCTION(BlueprintPure, Category="ML- Runtime")
	TArray<AML_Tile*> GetTreeTiles() const;

	UFUNCTION(BlueprintPure, Category="ML- Hex Grid")
	AML_Tile* FindClosestWalkableBorderTile(const FVector& WorldLocation) const;

	UFUNCTION(BlueprintPure, Category="ML- Hex Grid")
	AML_CameraRail* GetClosestCameraRail(const FVector& WorldLocation) const;

	UFUNCTION(BlueprintPure, Category="ML- Runtime")
	TArray<AActor*> GetExitGrounds() const { return ExitGrounds; }
	
	UFUNCTION(BlueprintPure, Category="ML- Runtime")
	int32 GetEnergyForPuzzle() const { return EnergyForPuzzle; }

	UFUNCTION(BlueprintPure, Category="ML- Runtime")
	UML_BiomeTileSet* GetBiomeTileSet() const { return BiomeTileSet; }

	UFUNCTION(BlueprintPure, Category="ML- Runtime")
	ACameraActor* GetAssociatedCamera() const { return AssociatedCamera; }

	UFUNCTION(BlueprintPure, Category="ML- Runtime")
	AActor* GetAssociatedObstacle() const { return AssociatedObstacle; }

	UFUNCTION(BlueprintPure, Category="ML- Runtime")
	ULevelSequence* GetAssociatedWinCinematic() const { return AssociatedWinCinematic; }

	UFUNCTION(BlueprintPure, Category="ML- Runtime")
	AML_CinematicCameraRail* GetAssociatedStartCinematicRail() const { return AssociatedStartCinematicRail; }

	UFUNCTION(BlueprintPure, Category="ML- Runtime")
	TArray<AActor*> GetAssociatedNatureZones() const { return AssociatedNatureZones; }

	UFUNCTION(BlueprintPure, Category="ML- Runtime")
	TArray<AActor*> GetAssociatedWaterPaths() const { return AssociatedWaterPaths; }

	// ==================== ML- Board Switches ====================
	// Per-board ON/OFF switches (backing fields are private — see above).

	UFUNCTION(BlueprintPure, Category="ML- Board Switches")
	bool IsGlowEnabled() const { return bGlowEnabled; }

	UFUNCTION(BlueprintPure, Category="ML- Board Switches")
	bool IsBoardTransitionEnabled() const { return bBoardTransitionEnabled; }

	UFUNCTION(BlueprintPure, Category="ML- Board Switches")
	bool IsWorldBoard() const { return bIsWorldBoard; }

	// True when a path coming from another board must stop on this board's first tile: an open puzzle.
	// A world board never stops the player, and neither does a puzzle with its transition off (solved, or
	// locked by Required Puzzles) — it is walked across like terrain, as it always was.
	UFUNCTION(BlueprintPure, Category="ML- Board Switches")
	bool ShouldStopPlayerOnEntry() const { return !bIsWorldBoard && bBoardTransitionEnabled; }

	/** Enables/disables this board's glow. Clears any active glow immediately when turning OFF. */
	UFUNCTION(BlueprintCallable, Category="ML- Board Switches")
	void SetGlowEnabled(bool bEnabled);

	/** Enables/disables this board's entry/exit (tile-by-tile) system. Ejects the player to free movement when turning OFF while they are inside. */
	UFUNCTION(BlueprintCallable, Category="ML- Board Switches")
	void SetBoardTransitionEnabled(bool bEnabled);

	/** True once every board in RequiredPuzzles is solved (always true when the list is empty). */
	UFUNCTION(BlueprintPure, Category="ML- Board Switches")
	bool ArePrerequisitesSolved() const;

	/** Pushes this board's lock state (glow + board entry) from its prerequisites. No-op when the
	 *  list is empty or while Lock Unreached Puzzle Boards is off in the developer settings. */
	void RefreshLockState();

	/** Runs RefreshLockState() on every board of the world. Called after a win or a reset is
	 *  committed to the save, so boards gated on the puzzle that just changed follow immediately. */
	static void RefreshAllBoardLockStates(UWorld* World);

	/** What the prerequisite rule must do this pass, from the facts that decide it. Pure (no world
	 *  access) so the rule stays in one place and can be unit-tested; RefreshLockState() gathers the
	 *  facts and applies the answer. */
	static EML_BoardLockAction ResolveLockAction(bool bPrerequisitesSolved, bool bLockedByRule, bool bIsSolved);

	// ==================== ML- Board Exits (gamepad) ====================

	// Gamepad board exits: each entry maps a set of border tiles to a plane/actor to highlight.
	// This is pure per-board configuration — the availability broadcast lives on the player controller
	// (a single subscription point), so this board only needs to answer "which plane for this tile?".
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category="ML- Board Exits")
	TArray<FML_BoardExit> BoardExits;

	// Returns the first exit whose tiles contain PlayerTile, or nullptr. Used both to know which plane
	// to highlight when the player stands on an exit tile, and to resolve the exit target on confirm.
	const FML_BoardExit* FindExitForTile(const AML_Tile* PlayerTile) const;

	// Single source of truth for "is there a usable gamepad exit for PlayerTile?". Returns the exit plane only
	// when it exists AND is currently enabled — a designer disables a ground exit via SetActorEnableCollision(false),
	// the same signal that makes the mouse ground-trace ignore it. Both gamepad consumers (walk-out + highlight)
	// go through this so the glow and the actual exit can never disagree. Returns nullptr otherwise.
	AActor* GetActiveExitPlaneForTile(const AML_Tile* PlayerTile) const;

	// ==================== ML- Puzzle Generator ====================

	UPROPERTY(EditAnywhere, Category="ML- Puzzle Generator")
	FML_PuzzleGenerationSettings PuzzleGenerationSettings;

	// Restores this board's tiles to their authored initial state and re-enables the
	// obstacle. Does NOT write to the save — call ReplayPuzzle (or ResetPuzzle on the
	// subsystem) to commit the change. Used directly for cascade resets.
	UFUNCTION(BlueprintCallable, Category="Myceland Hex Grid")
	virtual void RestoreToInitialState();

	// Plays this board's AssociatedWinCinematic through the cinematic subsystem, if one is set.
	// Driven on load by AML_PlayerCharacter::ApplySavedSpawnPosition AFTER the player has been
	// teleported onto its board and CurrentTileOn is valid, so the OnCinematicFinished handlers
	// that read CurrentTileOn (e.g. BP_ProgressionManager) never fire before the player is on a tile.
	void PlayAssociatedWinCinematic() const;

	// ==================== ML- Debug ====================

	// Force-wins this board without solving it. Click during PIE with the board selected (or call
	// from Blueprint) to instantly fire the full win pipeline — OnWin, link glow, OnWinPathSettled,
	// save, and the win cinematic. Body compiles out in shipping.
	UFUNCTION(CallInEditor, BlueprintCallable, Category="ML- Debug", meta=(DisplayName="Debug Auto Win"))
	void DebugAutoWin();

	// Analyzes the current authored grid against the puzzle-generation settings (editor diagnostics).
	UFUNCTION(CallInEditor, Category="ML- Debug")
	void AnalyzeCurrentPuzzle();

	// Resets this puzzle (and every puzzle solved after it in this level) back to its initial state
	// and updates the save. Intended for editor and Blueprint use.
	UFUNCTION(CallInEditor, BlueprintCallable, Category="ML- Debug", meta=(DisplayName="Replay Puzzle (Reset to Initial State)"))
	void ReplayPuzzle();

protected:

	// ==================== Save / Load Helpers ====================

	// If false, BeginPlay skips the automatic solved-grid restore so a subclass can
	// own its own restore path (e.g. the hub, which must snapshot pre-change tile
	// state for reverts before repainting). Default: true.
	virtual bool ShouldAutoRestoreSolvedGrid() const { return true; }

	// Returns a snapshot of the current GridMap as a flat array of tile entries.
	TArray<FML_TileSaveEntry> SnapshotGrid() const;

	// Fetches the SaveSubsystem from the owning GameInstance. Returns null if unavailable.
	UML_SaveSubsystem* GetSaveSubsystem() const;

	// Returns a stable level key (map name with PIE prefix stripped) used to scope
	// solve orders and cascade resets to the current level.
	FName GetLevelKey() const;

private:

	// Bound to UML_WinLoseSubsystem::OnWin; captures the solved grid and saves to disk.
	UFUNCTION()
	void HandlePuzzleWon();
};
