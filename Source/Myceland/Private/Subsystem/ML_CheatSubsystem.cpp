// Copyright Myceland Team, All Rights Reserved.

#include "Subsystem/ML_CheatSubsystem.h"

#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "EnhancedInputSubsystems.h"
#include "Actors/ML_CheatTeleportPoint.h"
#include "Blueprint/UserWidget.h"
#include "Component/ML_EnergyComponent.h"
#include "Components/CapsuleComponent.h"
#include "Developer Settings/ML_MycelandDeveloperSettings.h"
#include "GameFramework/PlayerStart.h"
#include "Player/ML_PlayerCharacter.h"
#include "Player/ML_PlayerController.h"
#include "Subsystem/ML_CinematicSubsystem.h"
#include "Subsystem/ML_NarrativeSubsystem.h"
#include "Subsystem/ML_UIManagerSubsystem.h"
#include "Subsystem/ML_WinLoseSubsystem.h"
#include "Tiles/ML_BoardSpawner.h"
#include "Tiles/ML_Tile.h"

#define LOCTEXT_NAMESPACE "MycelandCheats"

// ==================== Lifecycle ====================

UML_CheatSubsystem* UML_CheatSubsystem::Get(const UObject* WorldContextObject)
{
	if (!WorldContextObject) return nullptr;

	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UML_CheatSubsystem>() : nullptr;
}

void UML_CheatSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Cheat mode survives the level travel its own "go to level" cheat triggers, but the overlay
	// widget and the world do not - RestoreAfterTravel puts them back on the new world.
	FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &UML_CheatSubsystem::HandlePostLoadMap);
}

void UML_CheatSubsystem::Deinitialize()
{
	FCoreUObjectDelegates::PostLoadMapWithWorld.RemoveAll(this);

	if (bCheatModeActive)
	{
		ApplyCheatInputMapping(false);
		HideOverlay();
		bCheatModeActive = false;
	}

	Super::Deinitialize();
}

// ==================== Helpers ====================

const UML_MycelandDeveloperSettings* UML_CheatSubsystem::GetSettings() const
{
	return UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();
}

AML_PlayerController* UML_CheatSubsystem::GetPlayerController() const
{
	const UWorld* World = GetWorld();
	return World ? Cast<AML_PlayerController>(World->GetFirstPlayerController()) : nullptr;
}

bool UML_CheatSubsystem::CanRunCheat() const
{
	const UML_MycelandDeveloperSettings* Settings = GetSettings();
	return bCheatModeActive && Settings && Settings->bEnableCheats;
}

void UML_CheatSubsystem::Feedback(const FText& Message)
{
	UE_LOG(LogTemp, Log, TEXT("[Cheat] %s"), *Message.ToString());
	OnCheatFeedback.Broadcast(Message);
}

// ==================== Mode ====================

void UML_CheatSubsystem::ToggleCheatMode()
{
	SetCheatModeActive(!bCheatModeActive);
}

void UML_CheatSubsystem::SetCheatModeActive(bool bActive)
{
	const UML_MycelandDeveloperSettings* Settings = GetSettings();
	if (!Settings || !Settings->bEnableCheats)
		return;

	if (bCheatModeActive == bActive)
		return;

	bCheatModeActive = bActive;

	ApplyCheatInputMapping(bActive);

	if (bActive)
		ShowOverlay();
	else
		HideOverlay();

	OnCheatModeChanged.Broadcast(bActive);

	UE_LOG(LogTemp, Log, TEXT("[Cheat] Cheat mode %s"), bActive ? TEXT("ON") : TEXT("OFF"));
}

void UML_CheatSubsystem::ApplyCheatInputMapping(bool bEnable) const
{
	const UML_MycelandDeveloperSettings* Settings = GetSettings();
	if (!Settings) return;

	AML_PlayerController* PC = GetPlayerController();
	ULocalPlayer* LocalPlayer = PC ? PC->GetLocalPlayer() : nullptr;
	if (!LocalPlayer) return;

	UEnhancedInputLocalPlayerSubsystem* InputSub = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
	if (!InputSub) return;

	int32 Priority = 0;
	UInputMappingContext* CheatIMC = Settings->GetInputMappingContext(EInputMappingType::Cheat, Priority);
	if (!CheatIMC)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Cheat] No Cheat Input Mapping Context set in the Myceland Developer Settings."));
		return;
	}

	// Enhanced Input rebuilds its key mappings lazily, on the next input tick. The overlay is created
	// right after this call and asks QueryKeysMappedToAction which keys to display, so without forcing
	// the rebuild here it reads an empty table and shows a blank key column until a frame has passed.
	// bIgnoreAllPressedKeysUntilRelease stays true so the combo still held down (Ctrl+Shift+C) cannot
	// fire a cheat the instant the context is mapped.
	FModifyContextOptions Options;
	Options.bForceImmediately = true;

	if (bEnable)
		InputSub->AddMappingContext(CheatIMC, Priority, Options);
	else
		InputSub->RemoveMappingContext(CheatIMC, Options);
}

void UML_CheatSubsystem::ShowOverlay()
{
	if (IsValid(OverlayWidget))
		return;

	const UML_MycelandDeveloperSettings* Settings = GetSettings();
	if (!Settings || Settings->CheatOverlayWidgetClass.IsNull())
		return; // Optional: the cheat keys work fine without an overlay.

	AML_PlayerController* PC = GetPlayerController();
	if (!PC) return;

	UClass* WidgetClass = Settings->CheatOverlayWidgetClass.LoadSynchronous();
	if (!WidgetClass) return;

	OverlayWidget = CreateWidget<UUserWidget>(PC, WidgetClass);
	if (!OverlayWidget) return;

	// Straight to the viewport, on purpose: the overlay must draw over the HUD and the menus without
	// entering the UI manager's navigation stack or stealing the focus of whatever is open.
	OverlayWidget->AddToViewport(Settings->CheatOverlayZOrder);
}

void UML_CheatSubsystem::HideOverlay()
{
	if (IsValid(OverlayWidget))
		OverlayWidget->RemoveFromParent();

	OverlayWidget = nullptr;
}

// ==================== Level travel ====================

void UML_CheatSubsystem::HandlePostLoadMap(UWorld* LoadedWorld)
{
	// The old world took the overlay with it; the widget still points at a dead player controller.
	OverlayWidget = nullptr;

	if (!bCheatModeActive || !LoadedWorld)
		return;

	// One tick of slack so the new player controller exists and has possessed its pawn.
	LoadedWorld->GetTimerManager().SetTimerForNextTick(
		FTimerDelegate::CreateUObject(this, &UML_CheatSubsystem::RestoreAfterTravel));
}

void UML_CheatSubsystem::RestoreAfterTravel()
{
	if (!bCheatModeActive)
		return;

	ApplyCheatInputMapping(true);
	ShowOverlay();

	// The infinite-energy flag lives on the player controller's component, which is brand new.
	if (bInfiniteEnergy)
	{
		const UML_MycelandDeveloperSettings* Settings = GetSettings();
		AML_PlayerController* PC = GetPlayerController();
		if (Settings && PC && PC->EnergyComponent)
			PC->EnergyComponent->SetInfiniteEnergy(true, Settings->CheatInfiniteEnergyAmount);
	}
}

// ==================== Cheats ====================

void UML_CheatSubsystem::Cheat_TeleportToSlot(int32 Slot)
{
	if (!CanRunCheat()) return;

	AML_CheatTeleportPoint* Found = nullptr;
	for (AML_CheatTeleportPoint* Point : GatherTeleportPoints())
	{
		if (Point->Slot != Slot)
			continue;

		if (Found)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Cheat] Several teleport points share slot %d - using '%s'."),
				Slot, *Found->GetActorNameOrLabel());
			break;
		}

		Found = Point;
	}

	if (!Found)
	{
		Feedback(FText::Format(LOCTEXT("NoTeleportPoint", "No teleport point in slot {0} in this level"),
			FText::FromString(FString::FromInt(Slot))));
		return;
	}

	TeleportPlayerTo(Found->GetActorLocation(), Found->GetDisplayLabel());
}

void UML_CheatSubsystem::Cheat_OpenLevelSlot(int32 Slot)
{
	if (!CanRunCheat()) return;

	const TArray<FGameplayTag> Tags = GetSortedLevelTags();
	const int32 Index = Slot - 1;
	if (!Tags.IsValidIndex(Index))
	{
		Feedback(FText::Format(LOCTEXT("NoLevelSlot", "No level in slot {0}"),
			FText::FromString(FString::FromInt(Slot))));
		return;
	}

	UGameInstance* GameInstance = GetGameInstance();
	UML_UIManagerSubsystem* UIManager = GameInstance ? GameInstance->GetSubsystem<UML_UIManagerSubsystem>() : nullptr;
	AML_PlayerController* PC = GetPlayerController();
	if (!UIManager || !PC)
		return;

	Feedback(FText::Format(LOCTEXT("OpeningLevel", "Loading {0}..."),
		FText::FromString(Tags[Index].ToString())));

	UIManager->OpenLevelByTag(Tags[Index], PC);
}

void UML_CheatSubsystem::Cheat_WinCurrentPuzzle()
{
	if (!CanRunCheat()) return;

	const AML_PlayerController* PC = GetPlayerController();
	const AML_PlayerCharacter* Character = PC ? PC->GetMycelandCharacter() : nullptr;
	if (!IsValid(Character) || !IsValid(Character->CurrentTileOn))
	{
		Feedback(LOCTEXT("NotOnBoard", "The player is not on a board"));
		return;
	}

	AML_BoardSpawner* Board = Character->CurrentTileOn->GetBoardSpawnerFromTile();
	if (!IsValid(Board))
	{
		Feedback(LOCTEXT("NoBoard", "The player is not on a board"));
		return;
	}

	if (Board->bIsPuzzleSolved)
	{
		Feedback(LOCTEXT("AlreadySolved", "This puzzle is already solved"));
		return;
	}

	UWorld* World = GetWorld();
	UML_WinLoseSubsystem* WinLose = World ? World->GetSubsystem<UML_WinLoseSubsystem>() : nullptr;
	if (!WinLose)
		return;

	// Same entry point the hub board uses to award a puzzle: marks it solved and fires the win
	// sequence, so the nature zones revitalize exactly as they would on a real win.
	WinLose->ForceBoardWin(Board);

	Feedback(LOCTEXT("PuzzleWon", "Puzzle won"));
}

void UML_CheatSubsystem::Cheat_ToggleInfiniteEnergy()
{
	if (!CanRunCheat()) return;

	const UML_MycelandDeveloperSettings* Settings = GetSettings();
	AML_PlayerController* PC = GetPlayerController();
	if (!Settings || !PC || !PC->EnergyComponent)
		return;

	bInfiniteEnergy = !bInfiniteEnergy;
	PC->EnergyComponent->SetInfiniteEnergy(bInfiniteEnergy, Settings->CheatInfiniteEnergyAmount);

	Feedback(bInfiniteEnergy
		? LOCTEXT("InfiniteEnergyOn", "Infinite energy: ON")
		: LOCTEXT("InfiniteEnergyOff", "Infinite energy: OFF"));
}

void UML_CheatSubsystem::Cheat_ExitBoard()
{
	if (!CanRunCheat()) return;

	const AML_PlayerController* PC = GetPlayerController();
	const AML_PlayerCharacter* Character = PC ? PC->GetMycelandCharacter() : nullptr;
	UWorld* World = GetWorld();
	if (!IsValid(Character) || !World)
		return;

	const FVector PlayerLocation = Character->GetActorLocation();

	// Closest teleport point first: those are authored spots, so the player lands somewhere sane.
	const AML_CheatTeleportPoint* Closest = nullptr;
	double BestDistanceSq = TNumericLimits<double>::Max();
	for (const AML_CheatTeleportPoint* Point : GatherTeleportPoints())
	{
		const double DistanceSq = FVector::DistSquared(PlayerLocation, Point->GetActorLocation());
		if (DistanceSq < BestDistanceSq)
		{
			BestDistanceSq = DistanceSq;
			Closest = Point;
		}
	}

	if (Closest)
	{
		TeleportPlayerTo(Closest->GetActorLocation(), Closest->GetDisplayLabel());
		return;
	}

	// Fallback for a level with no teleport point authored yet.
	for (TActorIterator<APlayerStart> It(World); It; ++It)
	{
		TeleportPlayerTo(It->GetActorLocation(), LOCTEXT("PlayerStart", "Player Start"));
		return;
	}

	Feedback(LOCTEXT("NoUnstuckTarget", "No teleport point and no PlayerStart in this level"));
}

void UML_CheatSubsystem::Cheat_SkipNarrative()
{
	if (!CanRunCheat()) return;

	UWorld* World = GetWorld();
	UGameInstance* GameInstance = GetGameInstance();
	if (!World || !GameInstance)
		return;

	bool bSkippedSomething = false;

	if (UML_CinematicSubsystem* Cinematic = World->GetSubsystem<UML_CinematicSubsystem>())
	{
		if (Cinematic->IsCinematicPlaying())
		{
			Cinematic->StopCurrentCinematic();
			bSkippedSomething = true;
		}
	}

	// Stops the whole sequence rather than the current line: in a demo you want out of the dialogue,
	// not one line further into it. Cleanup restores player control on its own.
	if (UML_NarrativeSubsystem* Narrative = GameInstance->GetSubsystem<UML_NarrativeSubsystem>())
	{
		if (Narrative->IsSequencePlaying())
		{
			Narrative->StopSequence();
			bSkippedSomething = true;
		}
	}

	Feedback(bSkippedSomething
		? LOCTEXT("Skipped", "Cinematic / dialogue skipped")
		: LOCTEXT("NothingToSkip", "Nothing to skip"));
}

// ==================== Teleport ====================

void UML_CheatSubsystem::TeleportPlayerTo(const FVector& Destination, const FText& DestinationName)
{
	AML_PlayerController* PC = GetPlayerController();
	AML_PlayerCharacter* Character = PC ? PC->GetMycelandCharacter() : nullptr;
	if (!IsValid(Character) || !Character->GetCapsuleComponent())
		return;

	// Drop every in-progress path first, otherwise the movement the player had queued keeps ticking
	// and walks them straight back from wherever we just put them.
	PC->CancelAllMovementForTeleport();

	const float CapsuleHalfHeight = Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	Character->TeleportTo(Destination + FVector(0.f, 0.f, CapsuleHalfHeight + 10.f), Character->GetActorRotation());

	// Refresh the tile lookup now instead of waiting for the movement-based check in Tick: the board
	// change it broadcasts is what switches the movement mode to match the destination (free movement
	// outside a board, inside-board when the point sits on one).
	Character->UpdateCurrentTile();

	// Camera rails hand over through their trigger boxes and the board camera is picked at possession;
	// a teleport crosses no trigger, so the camera has to be resolved explicitly. After
	// UpdateCurrentTile on purpose: it is the board state it just refreshed that decides which camera.
	PC->ApplyCameraForCurrentLocation(0.f);

	Feedback(FText::Format(LOCTEXT("TeleportedTo", "Teleported to {0}"), DestinationName));
}

// ==================== Queries ====================

TArray<AML_CheatTeleportPoint*> UML_CheatSubsystem::GatherTeleportPoints() const
{
	TArray<AML_CheatTeleportPoint*> Points;

	UWorld* World = GetWorld();
	if (!World)
		return Points;

	for (TActorIterator<AML_CheatTeleportPoint> It(World); It; ++It)
	{
		if (IsValid(*It))
			Points.Add(*It);
	}

	// Actor iteration order is not stable, so sort: the overlay and the slot lookup must agree.
	Points.Sort([](const AML_CheatTeleportPoint& A, const AML_CheatTeleportPoint& B)
	{
		if (A.Slot != B.Slot)
			return A.Slot < B.Slot;

		return A.GetName() < B.GetName();
	});

	return Points;
}

TArray<FGameplayTag> UML_CheatSubsystem::GetSortedLevelTags() const
{
	TArray<FGameplayTag> Tags;

	const UML_MycelandDeveloperSettings* Settings = GetSettings();
	if (!Settings)
		return Tags;

	Settings->Levels.GetKeys(Tags);

	// Sorted by tag so the slot of a level is the same from one session to the next
	// (a TMap hands its keys back in insertion order, which config edits reshuffle).
	Tags.Sort([](const FGameplayTag& A, const FGameplayTag& B)
	{
		return A.ToString() < B.ToString();
	});

	return Tags;
}

// ==================== Overlay data ====================

FText UML_CheatSubsystem::ResolveKeyText(const UInputAction* Action) const
{
	if (!Action)
		return FText::GetEmpty();

	AML_PlayerController* PC = GetPlayerController();
	ULocalPlayer* LocalPlayer = PC ? PC->GetLocalPlayer() : nullptr;
	UEnhancedInputLocalPlayerSubsystem* InputSub = LocalPlayer ? LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
	if (!InputSub)
		return FText::GetEmpty();

	// Only reports keys of the mapping contexts currently mapped - which the cheat IMC is whenever
	// the overlay is up, so the list always shows the keys that actually work right now.
	// Short display name on purpose: "&" and "(" rather than "Ampersand" and "Left Parantheses".
	TArray<FString> KeyNames;
	for (const FKey& Key : InputSub->QueryKeysMappedToAction(Action))
		KeyNames.AddUnique(Key.GetDisplayName(false).ToString());

	return FText::FromString(FString::Join(KeyNames, TEXT(" / ")));
}

TMap<int32, FText> UML_CheatSubsystem::ResolveSlotKeyTexts(const UInputAction* Action) const
{
	TMap<int32, FText> SlotKeys;

	const UML_MycelandDeveloperSettings* Settings = GetSettings();
	if (!Action || !Settings)
		return SlotKeys;

	int32 Priority = 0;
	const UInputMappingContext* CheatIMC = Settings->GetInputMappingContext(EInputMappingType::Cheat, Priority);
	if (!CheatIMC)
		return SlotKeys;

	// One mapping per key, each carrying the Scalar modifier that turns the press into its slot
	// number (see the Teleport Slot action in the Dev Settings). Reading that modifier back is the
	// only way to know which key stands for which slot; a mapping without one feeds the raw axis
	// value, which is 1.
	TMap<int32, TArray<FString>> KeyNamesBySlot;
	for (const FEnhancedActionKeyMapping& Mapping : CheatIMC->GetMappings())
	{
		if (Mapping.Action != Action)
			continue;

		int32 Slot = 1;
		for (const UInputModifier* Modifier : Mapping.Modifiers)
		{
			if (const UInputModifierScalar* Scalar = Cast<UInputModifierScalar>(Modifier))
			{
				Slot = FMath::RoundToInt(Scalar->Scalar.X);
				break;
			}
		}

		KeyNamesBySlot.FindOrAdd(Slot).AddUnique(Mapping.Key.GetDisplayName(false).ToString());
	}

	for (const TPair<int32, TArray<FString>>& Pair : KeyNamesBySlot)
		SlotKeys.Add(Pair.Key, FText::FromString(FString::Join(Pair.Value, TEXT(" / "))));

	return SlotKeys;
}

FText UML_CheatSubsystem::FormatSlotKey(const FString& Format, int32 Slot) const
{
	FFormatOrderedArguments Args;
	Args.Add(FText::FromString(FString::FromInt(Slot)));
	return FText::Format(FTextFormat::FromString(Format), Args);
}

TArray<FML_CheatEntry> UML_CheatSubsystem::GetActionEntries() const
{
	TArray<FML_CheatEntry> Entries;

	const UML_MycelandDeveloperSettings* Settings = GetSettings();
	if (!Settings)
		return Entries;

	auto AddEntry = [&Entries](const FText& Keys, const FText& Label, const FText& Value)
	{
		FML_CheatEntry Entry;
		Entry.Keys = Keys;
		Entry.Label = Label;
		Entry.Value = Value;
		Entries.Add(MoveTemp(Entry));
	};

	AddEntry(ResolveKeyText(Settings->CheatWinPuzzleAction.LoadSynchronous()),
		LOCTEXT("WinPuzzleLabel", "Win the current puzzle"), FText::GetEmpty());

	AddEntry(ResolveKeyText(Settings->CheatInfiniteEnergyAction.LoadSynchronous()),
		LOCTEXT("InfiniteEnergyLabel", "Infinite energy"),
		bInfiniteEnergy ? LOCTEXT("On", "ON") : LOCTEXT("Off", "OFF"));

	AddEntry(ResolveKeyText(Settings->CheatExitBoardAction.LoadSynchronous()),
		LOCTEXT("ExitBoardLabel", "Leave the board (unstuck)"), FText::GetEmpty());

	AddEntry(ResolveKeyText(Settings->CheatSkipNarrativeAction.LoadSynchronous()),
		LOCTEXT("SkipLabel", "Skip cinematic / dialogue"), FText::GetEmpty());

	AddEntry(FText::FromString(Settings->CheatToggleKeyText),
		LOCTEXT("CloseLabel", "Close cheat mode"), FText::GetEmpty());

	return Entries;
}

TArray<FML_CheatEntry> UML_CheatSubsystem::GetTeleportEntries() const
{
	TArray<FML_CheatEntry> Entries;

	const UML_MycelandDeveloperSettings* Settings = GetSettings();
	if (!Settings)
		return Entries;

	const TMap<int32, FText> SlotKeys = ResolveSlotKeyTexts(Settings->CheatTeleportSlotAction.LoadSynchronous());

	for (const AML_CheatTeleportPoint* Point : GatherTeleportPoints())
	{
		const FText* ResolvedKeys = SlotKeys.Find(Point->Slot);

		FML_CheatEntry Entry;
		Entry.Keys = ResolvedKeys ? *ResolvedKeys : FormatSlotKey(Settings->CheatTeleportSlotKeyFormat, Point->Slot);
		Entry.Label = Point->GetDisplayLabel();
		Entries.Add(MoveTemp(Entry));
	}

	return Entries;
}

TArray<FML_CheatEntry> UML_CheatSubsystem::GetLevelEntries() const
{
	TArray<FML_CheatEntry> Entries;

	const UML_MycelandDeveloperSettings* Settings = GetSettings();
	if (!Settings)
		return Entries;

	const TMap<int32, FText> SlotKeys = ResolveSlotKeyTexts(Settings->CheatLevelSlotAction.LoadSynchronous());

	const TArray<FGameplayTag> Tags = GetSortedLevelTags();
	for (int32 Index = 0; Index < Tags.Num(); ++Index)
	{
		const FText* ResolvedKeys = SlotKeys.Find(Index + 1);

		FML_CheatEntry Entry;
		Entry.Keys = ResolvedKeys ? *ResolvedKeys : FormatSlotKey(Settings->CheatLevelSlotKeyFormat, Index + 1);
		Entry.Label = FText::FromString(Tags[Index].ToString());
		Entries.Add(MoveTemp(Entry));
	}

	return Entries;
}

#undef LOCTEXT_NAMESPACE
