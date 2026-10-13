// Copyright Myceland Team, All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Core/ML_CoreData.h"
#include "Engine/DeveloperSettings.h"
#include "InputMappingContext.h"
#include "ML_MycelandDeveloperSettings.generated.h"

class AML_Collectible;
class UML_PropagationWaves;
class UInputAction;
class UUserWidget;

USTRUCT(BlueprintType)
struct FML_LevelAmbiencePuzzleCount
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Ambience", meta=(Categories="Level"))
	FGameplayTag Level;

	// Total number of puzzle wins needed for this level to reach 100% living ambience.
	// Include the central hub as the final puzzle when the level uses one.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Ambience", meta=(ClampMin="1"))
	int32 PuzzleCount = 1;
};
UENUM(BlueprintType)
enum class EML_LevelAmbienceMode : uint8
{
	Normal      UMETA(DisplayName="Normal Progression"),
	DeadOnly    UMETA(DisplayName="Dead Only"),
	LivingOnly  UMETA(DisplayName="Living Only"),
	None        UMETA(DisplayName="None")
};

USTRUCT(BlueprintType)
struct FML_LevelFixedAmbience
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Ambience", meta=(Categories="Level"))
	FGameplayTag Level;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Ambience")
	EML_LevelAmbienceMode Mode = EML_LevelAmbienceMode::Normal;
};
USTRUCT(BlueprintType)
struct FML_LevelFixedMusic
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Music", meta=(Categories="Level"))
	FGameplayTag Level;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Music")
	FString MusicEventPath;
};
UCLASS(config=Game, defaultconfig, meta=(DisplayName="Myceland"))
class MYCELAND_API UML_MycelandDeveloperSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	// ==================== Input ====================

	// All gameplay IMCs, mapped together while not in a cinematic. Keep BOTH the mouse/keyboard and the
	// gamepad IMC here: device detection piggy-backs on their action callbacks, so both must stay mapped
	// for the gamepad <-> mouse/keyboard switch to work in both directions.
	UPROPERTY(EditAnywhere, config, Category="Input|IMC")
	TArray<FML_InputMappingEntry> GameplayInputMappingContexts;

	// IMC used during cinematic sequences — should contain only the skip action
	UPROPERTY(EditAnywhere, config, Category="Input|IMC")
	FML_InputMappingEntry CinematicInputMappingContext;

	UPROPERTY(EditAnywhere, config, Category="Input|IMC")
	FML_InputMappingEntry TeleportInputMappingContext;

	// Mouse/keyboard: how long the click must be held outside the board before the exit resolves. 0 = instant.
	// Shares the exit hold tick rate (ExitHoldTickRate) with the gamepad.
	UPROPERTY(EditAnywhere, config, Category="Input|Mouse", meta=(ClampMin="0.0", Tooltip="Mouse/keyboard: seconds the click must be held outside the board before leaving. 0 = instant. Shares ExitHoldTickRate with the gamepad."))
	float ExitBoardHoldDurationMouse = 0.f;

	// Gamepad: how long the stick must be held toward the exit plane before the exit resolves. 0 = instant.
	// Shares the exit hold tick rate (ExitHoldTickRate) with the mouse/keyboard.
	UPROPERTY(EditAnywhere, config, Category="Input|Gamepad", meta=(ClampMin="0.0", Tooltip="Gamepad: seconds the stick must be held toward the exit plane before leaving. 0 = instant. Shares ExitHoldTickRate with the mouse."))
	float ExitBoardHoldDurationGamepad = 0.f;

	// ---------- Gamepad · Left Stick (Move) ----------

	// Gamepad left stick (inside board): seconds the stick must be held before the player starts
	// auto-stepping to the next tile (the first tile is always stepped immediately on push).
	UPROPERTY(EditAnywhere, config, Category="Input|Gamepad|Left Stick (Move)", meta=(ClampMin="0.0", Tooltip="Seconds the left stick must be held before the player starts auto-stepping to the next tile (first step is immediate)."))
	float GamepadMoveHoldRepeatDelay = 0.3f;

	// Gamepad left stick: once auto-stepping is active, seconds between each step to the next tile.
	// Lower = faster.
	UPROPERTY(EditAnywhere, config, Category="Input|Gamepad|Left Stick (Move)", meta=(ClampMin="0.01", Tooltip="Seconds between each tile step while the left stick is held (lower = faster)."))
	float GamepadMoveHoldRepeatInterval = 0.1f;

	// Gamepad left stick (inside board): minimum stick/direction alignment (dot product) required to step
	// the player toward a neighbor tile. cos 60 deg = 0.5, cos 30 deg = 0.866.
	UPROPERTY(EditAnywhere, config, Category="Input|Gamepad|Left Stick (Move)", meta=(ClampMin="0.0", ClampMax="1.0", Tooltip="Minimum alignment (dot product) for the left stick to step toward a neighbor tile. 0.5 = cos 60 deg."))
	float GamepadMoveAlignmentThreshold = 0.5f;

	// ---------- Gamepad · Right Stick (Select) ----------

	// Gamepad right stick (inside board): seconds the stick must be held before the selection cursor starts
	// auto-advancing to the next tile (the first tile is always selected immediately on push).
	UPROPERTY(EditAnywhere, config, Category="Input|Gamepad|Right Stick (Select)", meta=(ClampMin="0.0", Tooltip="Seconds the right stick must be held before the tile selection starts auto-advancing (first selection is immediate)."))
	float GamepadSelectHoldRepeatDelay = 0.3f;

	// Gamepad right stick: once auto-advance is active, seconds between each step to the next tile.
	// Lower = faster. Acts as the hold movement speed of the selection cursor.
	UPROPERTY(EditAnywhere, config, Category="Input|Gamepad|Right Stick (Select)", meta=(ClampMin="0.01", Tooltip="Seconds between each tile step while the right stick is held (lower = faster)."))
	float GamepadSelectHoldRepeatInterval = 0.1f;

	// Gamepad right stick (inside board): minimum stick/direction alignment (dot product) required to
	// select a tile around the player. cos 60 deg = 0.5, cos 30 deg = 0.866.
	UPROPERTY(EditAnywhere, config, Category="Input|Gamepad|Right Stick (Select)", meta=(ClampMin="0.0", ClampMax="1.0", Tooltip="Minimum alignment (dot product) for the right stick to select a tile. 0.5 = cos 60 deg."))
	float GamepadSelectAlignmentThreshold = 0.5f;

	UPROPERTY(EditAnywhere, Config, Category="Input|Gamepad|Right Stick (Select)", meta=(ClampMin="1", Tooltip="The distance from the player a tile can be selected with the gamepad."))
	uint8 GamepadSelectRingDistance = 1;



	// ==================== User Settings · Graphics whitelists ====================

	// Resolutions offered in the graphics settings dropdown. The saved resolution
	// snaps to the closest entry when the game boots (ValidateSettings).
	UPROPERTY(EditAnywhere, config, Category="User Settings|Graphics")
	TArray<FIntPoint> ValidResolutions = {
		FIntPoint(1280, 720),
		FIntPoint(1600, 900),
		FIntPoint(1920, 1080),
		FIntPoint(2560, 1440),
		FIntPoint(3840, 2160)
	};

	// Frame rate limits offered in the graphics settings dropdown. 0 = Unlimited.
	UPROPERTY(EditAnywhere, config, Category="User Settings|Graphics")
	TArray<float> ValidFrameLimits = {
		30.f,
		60.f,
		120.f,
		144.f,
		0.f
	};



	// ==================== Performance · Tick Rates ====================

	// Every cursor detection timer shares this rate: tile hover preview (glow + path preview)
	// and ground hover detection (board exit).
	UPROPERTY(EditAnywhere, config, Category="Performance|Tick Rates", meta=(ClampMin="1.0", Units="Hz", Tooltip="Rate of the cursor detection timers (tile hover preview + ground hover). 30 Hz is enough for cursor feedback."))
	float CursorDetectionTickRate = 30.f;

	// Rate of the exit hold progression timer (the hold-to-leave-the-board gauge).
	UPROPERTY(EditAnywhere, config, Category="Performance|Tick Rates", meta=(ClampMin="1.0", Units="Hz", Tooltip="Rate of the exit hold progression timer (hold-to-leave-the-board gauge)."))
	float ExitHoldTickRate = 60.f;

	// Rate of the turn-toward-tile rotation timer (character turning to face the plant target).
	UPROPERTY(EditAnywhere, config, Category="Performance|Tick Rates", meta=(ClampMin="1.0", Units="Hz", Tooltip="Rate of the turn-toward-tile rotation timer (character turning to face the plant target)."))
	float TurnTowardTileTickRate = 60.f;

	// Timer-ready intervals (seconds). Clamped so a bad config value can never yield a zero/negative rate.
	float GetCursorDetectionTickInterval() const { return 1.f / FMath::Max(CursorDetectionTickRate, 1.f); }
	float GetExitHoldTickInterval() const { return 1.f / FMath::Max(ExitHoldTickRate, 1.f); }
	float GetTurnTowardTileTickInterval() const { return 1.f / FMath::Max(TurnTowardTileTickRate, 1.f); }



	// ==================== UI · Win / Lose ====================

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="UI|Win Lose")
	float TimeShowWinUI = 3.f;

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="UI|Win Lose")
	float DelayBeforeShowLoseUI = 0.f;



	// ==================== Narrative ====================

	// A dialogue line that has an FMOD event lasts as long as that event. Without one, its
	// duration is derived from the subtitle length, so a silent line stays readable instead of
	// starting and ending in the same frame (see UML_NarrativeSubsystem::StartLine).
	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Narrative|Subtitles", meta=(ClampMin="1.0", Tooltip="Reading speed used to time a dialogue line that has no FMOD event: duration = subtitle length / this value."))
	float SubtitleCharsPerSecond = 15.f;

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Narrative|Subtitles", meta=(ClampMin="0.0", Tooltip="Floor applied to the computed duration, so a very short line (\"What...\") still stays on screen long enough to be read."))
	float MinSubtitleDuration = 1.5f;

	// Seconds a dialogue line stays on screen when no sound is there to time it.
	// PreDelay and PostDelay are applied on top of this by the narrative subsystem.
	float GetSubtitleDuration(const FText& Subtitle) const
	{
		return FMath::Max(Subtitle.ToString().Len() / FMath::Max(SubtitleCharsPerSecond, 1.f), MinSubtitleDuration);
	}



	// ==================== Levels ====================

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Levels", meta=(ForceInlineRow, Categories="Level"))
	TMap<FGameplayTag, TSoftObjectPtr<UWorld>> Levels;



	// ==================== Audio ====================

	UPROPERTY(EditAnywhere, config, Category="Audio|FMOD VCA")
	FString MasterFMODVCAPath = TEXT("vca:/Master");

	UPROPERTY(EditAnywhere, config, Category="Audio|FMOD VCA")
	FString MusicFMODVCAPath = TEXT("vca:/Music");

	UPROPERTY(EditAnywhere, config, Category="Audio|FMOD VCA")
	FString SFXFMODVCAPath = TEXT("vca:/SFX");

	UPROPERTY(EditAnywhere, config, Category="Audio|FMOD VCA")
	FString VoiceFMODVCAPath = TEXT("vca:/Voice");

	UPROPERTY(EditAnywhere, config, Category="Audio|Ambience Enviro")
	TArray<FString> LivingAmbienceEventPaths;

	UPROPERTY(EditAnywhere, config, Category="Audio|Ambience Enviro")
	TArray<FString> DeadAmbienceEventPaths;

	UPROPERTY(EditAnywhere, config, Category="Audio|Ambience Enviro", meta=(ClampMin="0.0"))
	float AmbienceMinDelay = 3.0f;

	UPROPERTY(EditAnywhere, config, Category="Audio|Ambience Enviro", meta=(ClampMin="0.0"))
	float AmbienceMaxDelay = 5.0f;

	UPROPERTY(EditAnywhere, config, Category="Audio|Ambience Enviro")
	bool bAutoStartAmbienceEnviro = true;

	UPROPERTY(EditAnywhere, config, Category="Audio|Ambience Enviro")
	bool bFallbackToBoardSpawnerCount = true;

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Audio|Ambience Enviro", meta=(TitleProperty="Level"))
	TArray<FML_LevelAmbiencePuzzleCount> AmbiencePuzzleCounts;
	// Levels configured here override the normal dead -> living
	// puzzle-based environmental ambience progression.
	UPROPERTY(
		EditAnywhere,
		config,
		BlueprintReadOnly,
		Category="Audio|Ambience Enviro",
		meta=(TitleProperty="Level")
	)
	TArray<FML_LevelFixedAmbience> FixedAmbienceLevels;
	// ==================== Audio · Music Progression ====================

	// Ordered list of FMOD event paths (event:/...), one exclusive track per progression step.
	// Only one plays at a time: index 0 ("Musique 1") plays from 0 puzzles won, index 1
	// ("Musique 2") from 1 puzzle won, etc. Winning a puzzle switches the track instead of
	// layering on top of it. Progress past the last entry just keeps the last track playing.
	UPROPERTY(EditAnywhere, config, Category="Audio|Music Progression")
	TArray<FString> MusicTrackEventPaths;

	// Levels configured here ignore puzzle-based music progression
	// and keep one fixed FMOD event playing for the entire level.
	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Audio|Music Progression", meta=(TitleProperty="Level"))
	TArray<FML_LevelFixedMusic> FixedMusicLevels;

	UPROPERTY(EditAnywhere, config, Category="Audio|Music Progression")
	bool bAutoStartMusicProgression = true;

	// ---------- Ancienne logique (layers additifs) — conservée en commentaire pour référence ----------
	// UPROPERTY(EditAnywhere, config, Category="Audio|Music Layers")
	// TArray<FString> MusicLayerEventPaths;
	//
	// UPROPERTY(EditAnywhere, config, Category="Audio|Music Layers")
	// bool bAutoStartMusicLayers = true;



	// ==================== Gameplay · Propagation & Rollback ====================

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Gameplay|Propagation")
	TArray<FML_WavePriorityEntry> WavesPriority;

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Gameplay|Propagation", meta=(Tooltip="Delay between each global waves (grass, DELAY, parasite, DELAY, water, DELAY, etc..."))
	float InterWaveDelay = 1.f;

	UPROPERTY(EditAnywhere, config, BlueprintReadWrite, Category="Gameplay|Propagation", meta=(Tooltip="Delay between each tiles in a wave (tile distance 1 (from clicked tile), DELAY, distance 2, DELAY, etc...)"))
	float IntraWaveDelay = 0.3f;

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Gameplay|Propagation",
	meta=(ClampMin="0.0", Tooltip="Delay before a tile visually becomes Grass."))
	float GrassSpawnDelay = 0.3f;

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Gameplay|Propagation",
		meta=(ClampMin="0.0", Tooltip="Delay between Grass StartTransition and the tile becoming Parasite."))
	float GrassToParasiteDelay = 0.5f;

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Gameplay|Propagation",
		meta=(ClampMin="0.0", Tooltip="Safety net for the collectible spawn: a collectible waits for its source parasite to report the end of its transformation animation, and starts anyway after this delay if the report never comes."))
	float CollectibleSourceReadyTimeout = 4.f;

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Gameplay|Propagation",
		meta=(ClampMin="0.0", Tooltip="Safety net for the end of the collectible spawn flight: the collectible Blueprint reports it with Notify Spawn Animation Finished, and the collectible reports by itself after this delay if the call is not wired. Keep it close to the real flight duration. 0 disables it, and the wave settle timeout then covers it much more coarsely."))
	float CollectibleSpawnAnimationTimeout = 1.5f;

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Gameplay|Propagation",
		meta=(ClampMin="0.0", Tooltip="Safety net for a wave flagged Wait For Pending Visuals: if a Blueprint never reports the end of its animation, the wave starts anyway after this delay. 0 disables the safety net entirely (the wave then waits forever, which is only ever a debugging aid)."))
	float WaveVisualSettleTimeout = 6.f;

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Gameplay|Rollback")
	float UndoSpeed = 3.0f;

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Gameplay|Rollback", meta=(DisplayName="Undo Until Plant", Tooltip="When enabled, undo keeps going through Move actions until it also undoes the next Plant action."))
	bool bUndoUntilPlant = false;

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Gameplay|Rollback")
	float ResetSpeed = 3.0f;

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Gameplay|Rollback", meta=(DisplayName="Use Dynamic Rollback Speed", Tooltip="When enabled, undo and reset speeds are computed from the current rollback stack to target Reset Target Duration. When disabled, Undo Speed and Reset Speed are used directly."))
	bool bUseDynamicRollBackSpeed = true;

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Gameplay|Rollback", meta=(DisplayName="Rollback Minimum Speed", ClampMin="0.01", Tooltip="Minimum time dilation used by animated undo and reset."))
	float RollBackMinSpeed = 3.0f;

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Gameplay|Rollback", meta=(DisplayName="Rollback Maximum Speed", ClampMin="0.01", Tooltip="Maximum time dilation used by animated undo and reset. Limits movement speed to prevent the player from overshooting tiles or leaving the board."))
	float RollBackMaxSpeed = 20.0f;

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Gameplay|Rollback", meta=(ClampMin="0.1", Tooltip="Target real-time duration for a full animated reset. The reset time dilation is computed from the current undo stack so larger stacks rewind faster."))
	float ResetTargetDuration = 4.0f;


	// ==================== Performance · Frame Budgets ====================
	// Per-frame CPU budgets (milliseconds) for the subsystems that time-slice their
	// work across frames. When the work of one step exceeds the budget, the
	// remainder continues on the following frames.

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Performance|Frame Budgets", meta=(ClampMin="0.1", Tooltip="Max CPU time (milliseconds) the wave propagation may spend applying tile changes in a single frame. When a ring has more tiles than fit in the budget, the remaining tiles are applied on the following frames."))
	float WavePropagationFrameBudgetMs = 2.0f;

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Performance|Frame Budgets", meta=(ClampMin="0.1", Tooltip="Max CPU time (milliseconds) the animated undo/reset may spend reverting tiles and destroying spawned actors in a single frame. When an undo wave group is bigger than the budget, the remainder continues on the following frames."))
	float RollbackFrameBudgetMs = 8.0f;

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Performance|Frame Budgets", meta=(ClampMin="0.1", Tooltip="Max CPU time (milliseconds) the win propagation wave may spend applying tile changes in a single frame. When a ring has more tiles than fit in the budget, the remaining tiles are applied on the following frames."))
	float WinFrameBudgetMs = 2.0f;


	// ==================== Gameplay · Win ====================

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Gameplay|Win")
	float WinDelay = 0.5f;

	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Gameplay|Win", meta=(ToolTip="Delay between each glow tile to show the win path (connected goals)."))
	float WinTileDelay = 0.1f;


	// ==================== Gameplay · Progression ====================

	// Locks the puzzle boards a level has not reached yet. Which board waits for which is authored per
	// board, in its Required Puzzles list (ML- Board Switches) — see AML_BoardSpawner::RefreshLockState.
	// Turning this off gives their switches back to every board the rule had locked, so it is a safe way
	// to play a level as if nothing were gated.
	UPROPERTY(EditAnywhere, config, BlueprintReadOnly, Category="Gameplay|Progression", meta=(DisplayName="Lock Unreached Puzzle Boards", Tooltip="When enabled, a board that lists Required Puzzles shows no hover glow and cannot be entered until all of them are solved. A board with an empty Required Puzzles list is never locked."))
	bool bLockUnreachedPuzzleBoards = true;


	// ==================== Cheats ====================
	// Demo-only cheat mode (see UML_CheatSubsystem). Everything below is inert while Enable Cheats is
	// off: the toggle key is never even mapped, so a shipped build cannot open the mode at all.

	UPROPERTY(EditAnywhere, config, Category="Cheats", meta=(DisplayName="Enable Cheats", Tooltip="Master switch for the demo cheat mode. Leave OFF in the shipped build: while off the cheat toggle is never mapped and every cheat is a no-op."))
	bool bEnableCheats = false;

	// Mapped for the whole session and never removed - including during cinematics, so the skip cheat
	// stays reachable. Must contain ONLY the toggle action, on a combo hard to press by accident.
	UPROPERTY(EditAnywhere, config, Category="Cheats|IMC", meta=(Tooltip="IMC holding only the cheat mode toggle. Mapped for the whole session, so bind it to a deliberate combo (e.g. Ctrl+Alt+C via Chorded Action)."))
	FML_InputMappingEntry CheatToggleInputMappingContext;

	// Mapped only while cheat mode is active, at a high priority. The cheat keys do not exist outside
	// cheat mode, so simple keys here can never clash with gameplay.
	UPROPERTY(EditAnywhere, config, Category="Cheats|IMC", meta=(Tooltip="IMC holding every cheat action. Mapped only while cheat mode is active, so simple keys are safe here. Give it a priority above the gameplay IMCs."))
	FML_InputMappingEntry CheatInputMappingContext;

	// ---------- Cheats - Actions ----------
	// Bound in C++ by AML_PlayerController::BindCheatActions, so nothing has to be wired in Blueprint.

	UPROPERTY(EditAnywhere, config, Category="Cheats|Actions", meta=(Tooltip="Opens / closes cheat mode. Belongs to the Cheat Toggle IMC."))
	TSoftObjectPtr<UInputAction> CheatToggleAction;

	UPROPERTY(EditAnywhere, config, Category="Cheats|Actions", meta=(Tooltip="Axis1D. Teleports to the cheat teleport point whose Slot equals the action value: one mapping per key, each carrying a Scalar modifier of 1, 2, 3... So a single action covers all nine slots."))
	TSoftObjectPtr<UInputAction> CheatTeleportSlotAction;

	UPROPERTY(EditAnywhere, config, Category="Cheats|Actions", meta=(Tooltip="Axis1D. Opens the Nth level of the Levels map, same Scalar modifier trick as the teleport slots."))
	TSoftObjectPtr<UInputAction> CheatLevelSlotAction;

	UPROPERTY(EditAnywhere, config, Category="Cheats|Actions", meta=(Tooltip="Solves the board the player stands on and fires its win sequence."))
	TSoftObjectPtr<UInputAction> CheatWinPuzzleAction;

	UPROPERTY(EditAnywhere, config, Category="Cheats|Actions", meta=(Tooltip="Toggles infinite energy."))
	TSoftObjectPtr<UInputAction> CheatInfiniteEnergyAction;

	UPROPERTY(EditAnywhere, config, Category="Cheats|Actions", meta=(Tooltip="Unstuck: teleports the player to the closest cheat teleport point (a PlayerStart if there is none)."))
	TSoftObjectPtr<UInputAction> CheatExitBoardAction;

	UPROPERTY(EditAnywhere, config, Category="Cheats|Actions", meta=(Tooltip="Ends the running cinematic and dialogue sequence outright."))
	TSoftObjectPtr<UInputAction> CheatSkipNarrativeAction;

	// ---------- Cheats - UI ----------

	// Optional: cheat mode works without it, you just get no on-screen list.
	UPROPERTY(EditAnywhere, config, Category="Cheats|UI", meta=(Tooltip="Widget listing the available cheats, shown while cheat mode is active. Optional - the keys still work without it."))
	TSoftClassPtr<UUserWidget> CheatOverlayWidgetClass;

	UPROPERTY(EditAnywhere, config, Category="Cheats|UI", meta=(Tooltip="Z-order of the cheat overlay. High enough to draw over the HUD and the menus."))
	int32 CheatOverlayZOrder = 1000;

	// The slot keys are read back from the cheat IMC too (each mapping's Scalar modifier says which
	// slot its key stands for), so these two formats are only the fallback for a slot the IMC does
	// not map. {0} is the slot number.
	// The toggle is a chord (Ctrl+Alt+C and the like) and Enhanced Input reports the chord modifiers
	// as separate actions, so querying the IMC would only give back the final key. Written by hand here.
	UPROPERTY(EditAnywhere, config, Category="Cheats|UI", meta=(Tooltip="Combo shown in the overlay for closing cheat mode. Written by hand because chord modifiers cannot be read back from the IMC."))
	FString CheatToggleKeyText = TEXT("Ctrl+Alt+C");

	UPROPERTY(EditAnywhere, config, Category="Cheats|UI", meta=(Tooltip="Fallback text for a teleport slot the cheat IMC does not map - a mapped slot shows its real key. {0} is the slot number."))
	FString CheatTeleportSlotKeyFormat = TEXT("{0}");

	UPROPERTY(EditAnywhere, config, Category="Cheats|UI", meta=(Tooltip="Fallback text for a level slot the cheat IMC does not map - a mapped slot shows its real key. {0} is the slot number."))
	FString CheatLevelSlotKeyFormat = TEXT("F{0}");

	// ---------- Cheats - Gameplay ----------

	UPROPERTY(EditAnywhere, config, Category="Cheats|Gameplay", meta=(ClampMin="1", Tooltip="Energy stock the infinite-energy cheat floors the player at. Every path that would lower it (planting, changing board, a rollback) is clamped back up to this value."))
	int32 CheatInfiniteEnergyAmount = 99;



	// ==================== Helper ====================
	UFUNCTION(BlueprintCallable, Category="Myceland Settings")
	static void SetIntraWaveDelay(float NewDelay)
	{
		UML_MycelandDeveloperSettings* Settings =
			GetMutableDefault<UML_MycelandDeveloperSettings>();

		Settings->IntraWaveDelay = NewDelay;
	}
	UFUNCTION(BlueprintPure, Category="Myceland Settings")
	static const UML_MycelandDeveloperSettings* GetMycelandDeveloperSettings()
	{
		return GetDefault<UML_MycelandDeveloperSettings>();
	}

	UFUNCTION(BlueprintPure, Category="Myceland Settings")
	UInputMappingContext* GetInputMappingContext(EInputMappingType Type, int32& Priority) const
	{
		const FML_InputMappingEntry* Entry;

		switch (Type)
		{
			case EInputMappingType::Teleport:
				Entry = &TeleportInputMappingContext;
				break;
			case EInputMappingType::CheatToggle:
				Entry = &CheatToggleInputMappingContext;
				break;
			case EInputMappingType::Cheat:
				Entry = &CheatInputMappingContext;
				break;
			case EInputMappingType::Cinematic:
			default:
				Entry = &CinematicInputMappingContext;
				break;
		}

		Priority = Entry->Priority;

		// Resolve the soft reference: returns the asset if already in memory, loads it now
		// otherwise (IMCs are tiny assets). Returns nullptr if the entry is unset.
		return Entry->Mapping.LoadSynchronous();
	}
};
