// Copyright Myceland Team, All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ML_CoreData.h"
#include "Developer Settings/ML_MycelandDeveloperSettings.h"
#include "GameFramework/PlayerController.h"
#include "Component/ML_EnergyComponent.h"
#include "Component/ML_HoverPreviewComponent.h"
#include "Component/ML_MoveRecordingComponent.h"
#include "Component/ML_BoardTransitionComponent.h"
#include "Component/ML_NavigationBridgeComponent.h"
#include "Input/ML_InputDeviceManager.h"
#include "Input/Handlers/ML_MouseKeyboardInputHandler.h"
#include "Input/Handlers/ML_GamepadInputHandler.h"
#include "ML_PlayerController.generated.h"

class AML_CameraRail;
class UML_WidgetBase;
class UML_MycelandDeveloperSettings;
class UEnhancedInputLocalPlayerSubsystem;
class UEnhancedInputComponent;
struct FInputActionValue;
class AML_PlayerCharacter;
class AML_BoardSpawner;
class AML_Tile;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGrassPlanted, AML_Tile*, PlantedTile);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGrassPlantStarted, AML_Tile*, TargetTile);
// Single, controller-wide broadcast for gamepad board-exit availability. Boards only hold their exit
// config; the gamepad handler resolves the plane for the player's tile and broadcasts here so exit
// plane actors have ONE place to subscribe (Get Player Controller → Cast → bind this event) instead
// of subscribing to every board individually. Compare ExitPlane against self to know if it concerns you.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnBoardExitAvailabilityChanged, AActor*, ExitPlane, bool, bIsAvailable);

UCLASS()
class MYCELAND_API AML_PlayerController : public APlayerController
{
	GENERATED_BODY()

private:
	// ==================== References ====================

	UPROPERTY()
	const UML_MycelandDeveloperSettings* DevSettings;

	UPROPERTY()
	AML_PlayerCharacter* MycelandCharacter;

	// ==================== State ====================

	TArray<FVector> CurrentPathWorld;
	int32 CurrentPathIndex = 0;

	bool bIsMoving = false;
	bool bInCinematicMode = false;

	// Last device seen by HandleInputDeviceChanged — used to detect Gamepad→MK transitions.
	EML_InputDevice PreviousInputDevice = EML_InputDevice::MouseKeyboard;

	// When true, the next MK input action is silently consumed (cursor just reappeared).
	bool bShouldConsumeNextInput = false;

	// Cursor position saved when switching to gamepad, restored when showing the cursor again.
	FVector2D LockedCursorPos = FVector2D::ZeroVector;

	// ==================== Loading Screen ====================

	// Live splash instance (created in BeginPlay via ShowLoadingScreen, removed by HideLoadingScreen).
	UPROPERTY(Transient)
	TObjectPtr<UML_WidgetBase> LoadingScreenInstance = nullptr;

	FTimerHandle LoadingScreenTimerHandle;

	// Creates the loading splash (if LoadingScreenClass is set), adds it on top of the viewport, and
	// arms the auto-hide timer. No-op when LoadingScreenClass is unset.
	void ShowLoadingScreen();

	// Removes the loading splash from the viewport and clears the auto-hide timer. Safe to call twice.
	void HideLoadingScreen();

	// ==================== Movement - Path Tick & Callbacks ====================

	void TickMoveAlongPath(float DeltaTime);
	void OnPathFinished();
	void SetIsMoving(bool bNewIsMoving);

	// ==================== Movement - Path Management ====================

	bool StartRecordedBoardMove(const TArray<FIntPoint>& AxialPath, const TMap<FIntPoint, AML_Tile*>& GridMap,
		EML_PlayerBoardActionState ActionState = EML_PlayerBoardActionState::Moving, AML_Tile* PlantTarget = nullptr);

	/**
	 * Redirects the active in-progress world-space path to follow FullMergedAxialPath,
	 * while preserving the exact logical target index currently being aimed at.
	 */
	void ExtendMoveAlongPath(const TArray<FIntPoint>& FullMergedAxialPath, const TMap<FIntPoint, AML_Tile*>& GridMap,
		int32 PreservedPathIndex);

	// ==================== Movement - Tile Movement (private helpers) ====================

	void ExecutePlant(AML_Tile* HitTile);
	bool RejectPlantWithFeedback();

	// ==================== Camera Queries ====================

	AML_CameraRail* FindClosestCameraRailFromPlayer(const FVector& WorldLocation);

	// ==================== Input Mapping ====================

	UEnhancedInputLocalPlayerSubsystem* GetEnhancedInputSubsystem() const;

	// ==================== Cheats ====================
	// Demo-only, and entirely gated on Enable Cheats in the Myceland Developer Settings: with it off
	// nothing below is mapped or bound, so the cheat keys do not exist at all.

	/** Maps the cheat toggle IMC for the whole session. Never removed - including during a cinematic,
	 *  so the skip cheat stays reachable. */
	void ApplyCheatToggleInputMappingContext();

	void OnCheatToggle();
	void OnCheatTeleportSlot(const FInputActionValue& Value);
	void OnCheatLevelSlot(const FInputActionValue& Value);
	void OnCheatWinPuzzle();
	void OnCheatInfiniteEnergy();
	void OnCheatExitBoard();
	void OnCheatSkipNarrative();

	// ==================== Delegates ====================

	UFUNCTION()
	void HandleCurrentTileChanged(const AML_Tile* OldTile, const AML_Tile* NewTile);

	UFUNCTION()
	void HandleBoardStateChanged(const AML_Tile* OldTile, const AML_Tile* NewTile);

protected:
	// ==================== Lifecycle ====================

	AML_PlayerController();
	virtual void BeginPlay() override;
	virtual void PlayerTick(float DeltaTime) override;
	virtual void OnPossess(APawn* aPawn) override;

	// ==================== Input ====================

	// Bind to IA_Move → Started
	UFUNCTION(BlueprintCallable, Category = "Myceland Controller")
	void OnSetDestinationStarted();

	// Bind to IA_Move → Triggered
	UFUNCTION(BlueprintCallable, Category = "Myceland Controller")
	void OnSetDestinationTriggered();

	// Bind to IA_Move → Completed / Canceled
	UFUNCTION(BlueprintCallable, Category = "Myceland Controller")
	void OnSetDestinationReleased();

	// Bind to IA_MoveAndPlant → Started
	UFUNCTION(BlueprintCallable, Category = "Myceland Controller")
	void OnMoveAndPlantStarted();

	// Bind to IA_GamepadConfirm → Started. In-board this is the PLANT button: it plants the tile
	// currently selected by the right stick. (Movement is now handled directly by the left stick.)
	UFUNCTION(BlueprintCallable, Category = "Myceland Controller")
	void OnGamepadConfirmStarted();

	// Bind to IA_GamepadMove → Triggered (Axis2D, left stick)
	UFUNCTION(BlueprintCallable, Category = "Myceland Controller")
	void OnGamepadMoveAxis(const FVector2D& Value);

	UFUNCTION(BlueprintCallable, Category = "Myceland Controller")
	void OnGamepadMoveReleased();

	// Bind to IA_GamepadSelectPlant → Triggered (Axis2D, right stick) — selects a plantable tile
	UFUNCTION(BlueprintCallable, Category = "Myceland Controller")
	void OnGamepadSelectPlantAxis(const FVector2D& Value);

	// Bind to IA_GamepadSelectPlant → Completed / Canceled
	UFUNCTION(BlueprintCallable, Category = "Myceland Controller")
	void OnGamepadSelectPlantReleased();

	UFUNCTION(BlueprintCallable, Category = "Myceland Controller")
	void OnSkipNarrativeLine();

	// Reacts to input device switches to update cursor visibility.
	UFUNCTION()
	void HandleInputDeviceChanged(EML_InputDevice NewDevice);

	// ==================== Movement Tuning ====================

	UPROPERTY(EditAnywhere, Category = "Myceland|Movement")
	float AcceptanceRadius = 12.f;

	UPROPERTY(EditAnywhere, Category = "Myceland|Movement")
	float MoveSpeedScale = 1.f;

	UPROPERTY(EditAnywhere, Category = "Myceland|Movement")
	float RotateSpeed = 10.f;

	// 0 = strict center-to-center, 1 = maximum smoothing
	UPROPERTY(EditAnywhere, Category = "Myceland|Movement|Smoothing", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CornerCutStrength = 0.5f;

	UPROPERTY(EditAnywhere, Category = "Myceland|Movement|Smoothing", meta = (ClampMin = "0.0"))
	float BaseCornerCutDistance = 80.f;

	// Nav Mesh Movement
	UPROPERTY(EditAnywhere, Category = "Myceland|Movement|NavMesh")
	float NavMeshAcceptanceRadius = 50.f;

	UPROPERTY(EditAnywhere, Category = "Myceland|Movement")
	float ShortPressThreshold = 0.5f;

	// ==================== Loading Screen ====================

	// Splash widget shown on load (level begin) and hidden after LoadingScreenDuration seconds.
	// Assign WB_LoadingScreen (or any UML_WidgetBase) in the controller Blueprint's Class Defaults.
	// Leave unset to disable the splash entirely.
	UPROPERTY(EditDefaultsOnly, Category = "Myceland|Loading Screen")
	TSubclassOf<UML_WidgetBase> LoadingScreenClass;

	// How long the splash stays up, in seconds. <= 0 keeps it up until HideLoadingScreen is called.
	UPROPERTY(EditDefaultsOnly, Category = "Myceland|Loading Screen", meta = (ClampMin = "0.0"))
	float LoadingScreenDuration = 10.f;

public:
	// ==================== Tile Query ====================

	AML_Tile* GetTileUnderCursor() const;

	static AML_Tile* ExtractTileFromHit(const FHitResult& Hit);

	// ==================== Ground Validation ====================

	bool IsClickableGround(const FHitResult& Hit) const;

	/**
	 * Traces on the dedicated Ground channel (ECC_GameTraceChannel2). Returns true only when the
	 * cursor is over a designated ground surface (landscape / exit plane set to Block that channel) —
	 * never the board (tiles read as "over the board") nor decor (ignores the channel by default).
	 * Used exclusively for the board exit detection while InsideBoard/ExitingBoard.
	 */
	bool GetGroundUnderCursor(FHitResult& OutHit) const;

	// ==================== Character Access ====================

	AML_PlayerCharacter* GetMycelandCharacter() const { return MycelandCharacter; }

	// ==================== Movement - Tile Movement ====================

	bool Move(AML_Tile* TargetTile, int32 StopBeforeTarget = 0);
	bool Plant(AML_Tile* TargetTile);

	// ==================== Components ====================

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Myceland Controller|Components")
	UML_EnergyComponent* EnergyComponent = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Myceland Controller|Components")
	UML_HoverPreviewComponent* HoverPreviewComponent = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Myceland Controller|Components")
	UML_MoveRecordingComponent* MoveRecordingComponent = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Myceland Controller|Components")
	UML_BoardTransitionComponent* TransitionComponent = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Myceland Controller|Components")
	UML_NavigationBridgeComponent* NavigationBridgeComponent = nullptr;

	// ==================== Input Components ====================

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Myceland Controller|Input")
	UML_InputDeviceManager* InputDeviceManager = nullptr;
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Myceland Controller|Input")
	UML_MouseKeyboardInputHandler* MouseKeyboardHandler = nullptr;
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Myceland Controller|Input")
	UML_GamepadInputHandler* GamepadHandler = nullptr;

	// ==================== Delegates ====================

	// Called when the player starts turning toward a tile to plant
	UPROPERTY(BlueprintAssignable, Category = "Myceland Controller|Plant")
	FOnGrassPlantStarted OnGrassPlantStarted;
	
	// Called when grass is successfully planted on a tile
	UPROPERTY(BlueprintAssignable, Category = "Myceland Controller|Plant")
	FOnGrassPlanted OnGrassPlanted;

	// Gamepad: broadcast when the player stands on / leaves a board exit tile. Subscribe from the exit
	// plane actor and highlight when ExitPlane == self. Single endpoint for every board's exits.
	UPROPERTY(BlueprintAssignable, Category = "Myceland Controller|Board Exit")
	FOnBoardExitAvailabilityChanged OnBoardExitAvailabilityChanged;

	// ==================== Camera ====================

	UFUNCTION(BlueprintCallable, Category="Myceland Controller|Camera")
	void BlendToViewTarget(AActor* NewViewTarget, float BlendTime = 2.f, float BlendExp = 0.f, EViewTargetBlendFunction BlendFunc = VTBlend_Linear);

	/**
	 * Makes the camera match where the player currently stands: the board's associated camera while
	 * inside a board, the closest camera rail otherwise. Used at possession, and after a teleport —
	 * rails hand over through their trigger boxes, which a teleport never crosses, so without this
	 * the player lands under whatever camera they had before.
	 */
	void ApplyCameraForCurrentLocation(float BlendTime = 0.f);

	// ==================== Movement Control ====================

	/** Called by TransitionComponent (ConfirmExitBoard, HandlePathFinished). */
	void StartMoveAlongPath(const TArray<FIntPoint>& AxialPath, const TMap<FIntPoint, AML_Tile*>& GridMap);

	/** Called by TransitionComponent (ConfirmExitBoard, HandlePathFinished). */
	void StartNavMeshMovement(const FVector& WorldLocation);

	/** Called by TransitionComponent to change the movement mode and notify other systems. */
	void SetMovementMode(EML_PlayerMovementMode NewMode);

	// ==================== Callbacks & State Management ====================

	/** Called by TransitionComponent when turn-toward-tile completes. */
	void ConfirmTurn(AML_Tile* HitTile);

	void NotifyGrassPlantStarted(AML_Tile* TargetTile);
	
	/**
	 * Binds the cheat actions declared in the Dev Settings (nothing to wire in Blueprint).
	 *
	 * Called by AML_PlayerCharacter with the PAWN's input component on purpose, not with the
	 * controller's: the loading screen, the board lock, the cinematics and the rollback all call
	 * DisableInput on the controller, which takes its input component off the input stack. Bound
	 * there, the cheats would be dead in exactly the situations they exist to get you out of.
	 */
	void BindCheatActions(UEnhancedInputComponent* EnhancedInputComponent);

	void UpdateCursorVisibility(const bool bVisible);
	void NotifyCinematicModeChanged(const bool bInCinematicMode);

	/**
	 * Maps every configured gameplay IMC (GameplayInputMappingContexts) at once. Called on possession and
	 * when a cinematic ends (the Narrative subsystem removes them while the Cinematic IMC is active, then
	 * calls this to restore them). Device detection does NOT depend on these — see the detection IMC.
	 */
	void ApplyGameplayInputMappingContext();

	/** Removes every configured gameplay IMC. Called by the Narrative subsystem when a cinematic starts. */
	void RemoveGameplayInputMappingContexts();

	// ==================== Actions ====================

	UFUNCTION(BlueprintCallable, Category="Myceland Controller")
	bool MovePlayerToAxial(const FIntPoint& TargetAxial, bool bUsePath, bool bFallbackTeleport, const FVector& TeleportFallbackWorld);

	UFUNCTION(BlueprintCallable, Category="Myceland Controller|Undo")
	void StartMoveAlongAxialPathForUndo(const TArray<FIntPoint>& AxialPath, const TArray<FIntPoint>& PickedCollectibleAxials);

	void NotifyCollectiblePickedOnAxial(const FIntPoint& Axial);

	// ==================== Queries ====================

	bool IsMoveInProgress() const { return MoveRecordingComponent && MoveRecordingComponent->IsMoveInProgress(); }
	bool IsUndoMovePlayback() const { return MoveRecordingComponent && MoveRecordingComponent->IsUndoMovePlayback(); }

	/** True while the gamepad is the active input device (used e.g. to keep the cursor hidden in menus). */
	UFUNCTION(BlueprintPure, Category = "Myceland Controller|Input")
	bool IsGamepadActive() const { return InputDeviceManager && InputDeviceManager->GetCurrentDevice() == EML_InputDevice::Gamepad; }
	
	float GetMoveSpeedScale() const { return MoveSpeedScale; }
	float GetShortPressThreshold() const { return ShortPressThreshold; }

	// ==================== Component Wrappers ====================
	// These allow components to call through the controller without cross-component includes.

	EML_PlayerMovementMode GetMovementMode() const { return TransitionComponent ? TransitionComponent->GetMovementMode() : EML_PlayerMovementMode::FreeMovement; }
	EML_PlayerBoardActionState GetBoardActionState() const { return TransitionComponent ? TransitionComponent->GetBoardActionState() : EML_PlayerBoardActionState::Idle; }
	bool IsHoldingExitInput() const { return TransitionComponent && TransitionComponent->IsHoldingExitInput(); }
	bool HasEnergy()const { return EnergyComponent && EnergyComponent->GetCurrentEnergy() > 0; }

	void RequestExitHold(AML_Tile* ExitBorderTile, const FVector& WorldTarget);
	void CancelExitHold();
	void RequestBoardEntry(AML_Tile* TargetTile);
	void StopNavMeshMovement();

	/**
	 * Free-movement hold: navmesh-aware steering direction toward the cursor.
	 * See UML_NavigationBridgeComponent::GetNavSteeringDirection for the return contract.
	 */
	bool GetNavSteeringDirection(const FVector& Destination, float DeltaTime, FVector& OutDirection);
	void ResetNavSteering();

	/** Stops NavMesh movement and cancels any pending board entry. Called when a cinematic interrupts navigation. */
	void CancelPendingNavigation();

	/**
	 * Hard-stops every in-progress movement: the navmesh move, a pending board entry AND the
	 * tile-by-tile board path. Used by the cheat teleport - without dropping the board path, the
	 * queued movement keeps ticking and walks the player back from wherever they were teleported.
	 */
	void CancelAllMovementForTeleport();
	AML_Tile* FindReachableExitBorderTile(const AML_BoardSpawner* Board, const FVector& OutsideDestination) const;
	AML_Tile* PredictNavMeshEntryTile(const AML_BoardSpawner* Board, const FVector& Destination) const;
	void SetForcedHoverTile(AML_Tile* Tile);
	void ClearForcedHoverTile();

	/** Gamepad plant selection (forwarded to the HoverPreviewComponent, which owns the board visuals). */
	void SetGamepadSelectedTile(AML_Tile* Tile);
	AML_Tile* GetGamepadSelectedTile() const;
	void ClearPathHoverPreview();

	/** Clears the currently active glow (cursor + path). Called by AML_BoardSpawner when glow is toggled OFF. */
	void ClearActiveGlow();

	/**
	 * Called by AML_BoardSpawner when its transition is toggled OFF. If the player is currently inside
	 * that board, stops any board movement and ejects them back to free movement.
	 */
	void NotifyBoardTransitionDisabled(const AML_BoardSpawner* Board);
};
