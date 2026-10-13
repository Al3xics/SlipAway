// Copyright Myceland Team, All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "ML_CheatSubsystem.generated.h"

class AML_CheatTeleportPoint;
class AML_PlayerController;
class UInputAction;
class UML_MycelandDeveloperSettings;
class UUserWidget;

/** One row of the cheat overlay. Built here so the widget only has to draw what it is handed. */
USTRUCT(BlueprintType)
struct FML_CheatEntry
{
	GENERATED_BODY()

	// Key(s) to press, all resolved from the cheat mapping context: by action for the fixed cheats,
	// by Scalar modifier for the teleport / level slots. Falls back to the key format strings of the
	// Cheats section of the Dev Settings when a slot has no mapping.
	UPROPERTY(BlueprintReadOnly, Category = "Cheats")
	FText Keys;

	// What pressing it does.
	UPROPERTY(BlueprintReadOnly, Category = "Cheats")
	FText Label;

	// Current state of a toggle ("ON" / "OFF"). Empty for everything else.
	UPROPERTY(BlueprintReadOnly, Category = "Cheats")
	FText Value;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCheatModeChanged, bool, bIsActive);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCheatFeedback, const FText&, Message);

/**
 * Demo-only cheat mode.
 *
 * Everything here is inert unless bEnableCheats is ticked in the Myceland Developer Settings, so a
 * shipped build never even maps the toggle key. A GameInstance subsystem (not a world one) because
 * the mode has to survive the level travel its own "go to level" cheat triggers.
 *
 * Input lives in two mapping contexts: the toggle IMC is mapped for the whole session by
 * AML_PlayerController (including during cinematics, so the skip cheat stays reachable), while the
 * cheat IMC is mapped here only while the mode is active — the cheat keys simply do not exist
 * outside cheat mode and can never clash with gameplay.
 */
UCLASS()
class MYCELAND_API UML_CheatSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	static UML_CheatSubsystem* Get(const UObject* WorldContextObject);

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ==================== Mode ====================

	UFUNCTION(BlueprintCallable, Category = "Myceland Cheats")
	void ToggleCheatMode();

	UFUNCTION(BlueprintCallable, Category = "Myceland Cheats")
	void SetCheatModeActive(bool bActive);

	UFUNCTION(BlueprintPure, Category = "Myceland Cheats")
	bool IsCheatModeActive() const { return bCheatModeActive; }

	// ==================== Cheats ====================
	// All of them no-op while cheat mode is off, so a stray Blueprint call can never fire one.

	/** Teleports the player to the AML_CheatTeleportPoint whose Slot matches, in the current level. */
	UFUNCTION(BlueprintCallable, Category = "Myceland Cheats|Actions")
	void Cheat_TeleportToSlot(int32 Slot);

	/** Opens the Nth level of the Dev Settings "Levels" map (sorted by tag, same order as the overlay). */
	UFUNCTION(BlueprintCallable, Category = "Myceland Cheats|Actions")
	void Cheat_OpenLevelSlot(int32 Slot);

	/** Marks the board the player stands on as solved and fires its win sequence (revitalization included). */
	UFUNCTION(BlueprintCallable, Category = "Myceland Cheats|Actions")
	void Cheat_WinCurrentPuzzle();

	UFUNCTION(BlueprintCallable, Category = "Myceland Cheats|Actions")
	void Cheat_ToggleInfiniteEnergy();

	/** Unstuck: teleports the player to the closest teleport point, or to a PlayerStart as a fallback. */
	UFUNCTION(BlueprintCallable, Category = "Myceland Cheats|Actions")
	void Cheat_ExitBoard();

	/** Ends the running cinematic and/or dialogue sequence outright (not just the current line). */
	UFUNCTION(BlueprintCallable, Category = "Myceland Cheats|Actions")
	void Cheat_SkipNarrative();

	// ==================== Overlay data ====================

	/** The fixed cheats, with their key resolved live from the mapping context. */
	UFUNCTION(BlueprintPure, Category = "Myceland Cheats|UI")
	TArray<FML_CheatEntry> GetActionEntries() const;

	/** The teleport points of the current level, ordered by slot. */
	UFUNCTION(BlueprintPure, Category = "Myceland Cheats|UI")
	TArray<FML_CheatEntry> GetTeleportEntries() const;

	/** The levels of the Dev Settings, ordered by tag. */
	UFUNCTION(BlueprintPure, Category = "Myceland Cheats|UI")
	TArray<FML_CheatEntry> GetLevelEntries() const;

	// ==================== Delegates ====================

	UPROPERTY(BlueprintAssignable, Category = "Myceland Cheats")
	FOnCheatModeChanged OnCheatModeChanged;

	// Short message describing what the last cheat did (or why it did nothing). Meant to be
	// displayed for a couple of seconds by the overlay.
	UPROPERTY(BlueprintAssignable, Category = "Myceland Cheats")
	FOnCheatFeedback OnCheatFeedback;

private:
	bool bCheatModeActive = false;
	bool bInfiniteEnergy = false;

	UPROPERTY(Transient)
	UUserWidget* OverlayWidget = nullptr;

	FTimerHandle RestoreAfterTravelTimer;

	// ---------- Helpers ----------

	const UML_MycelandDeveloperSettings* GetSettings() const;
	AML_PlayerController* GetPlayerController() const;

	// True when the cheats are usable at all (master switch ticked and mode active).
	bool CanRunCheat() const;

	void ApplyCheatInputMapping(bool bEnable) const;
	void ShowOverlay();
	void HideOverlay();

	void Feedback(const FText& Message);

	// Cancels every in-progress movement, moves the player, then refreshes the tile/board state so
	// the movement mode follows the destination immediately.
	void TeleportPlayerTo(const FVector& Destination, const FText& DestinationName);

	TArray<AML_CheatTeleportPoint*> GatherTeleportPoints() const;
	TArray<FGameplayTag> GetSortedLevelTags() const;

	FText ResolveKeyText(const UInputAction* Action) const;

	// The slot lists share a single Axis1D action for their nine keys, so QueryKeysMappedToAction
	// cannot tell which key means which slot: the mappings of the cheat IMC are walked instead, each
	// one's Scalar modifier giving the slot its key stands for. Keyed by slot number.
	TMap<int32, FText> ResolveSlotKeyTexts(const UInputAction* Action) const;

	// Fallback for a slot the IMC does not map (or maps without a Scalar modifier).
	FText FormatSlotKey(const FString& Format, int32 Slot) const;

	// Cheat mode survives a level travel, but the overlay widget and the world do not: re-apply
	// the mapping, the overlay and the infinite-energy flag on the freshly loaded world.
	void HandlePostLoadMap(UWorld* LoadedWorld);
	void RestoreAfterTravel();
};
