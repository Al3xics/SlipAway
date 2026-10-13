// Copyright Myceland Team, All Rights Reserved.

#include "Subsystem/ML_AmbienceSubsystem.h"

#include "Developer Settings/ML_MycelandDeveloperSettings.h"
#include "Kismet/GameplayStatics.h"
#include "Subsystem/ML_SoundSubsystem.h"
#include "Subsystem/ML_WinLoseSubsystem.h"
#include "FMODAudioComponent.h"
#include "Tiles/ML_BoardSpawner.h"

DEFINE_LOG_CATEGORY_STATIC(LogMycelandAmbience, Log, All);

void UML_AmbienceSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	DevSettings = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();
	WinLoseSubsystem = InWorld.GetSubsystem<UML_WinLoseSubsystem>();

	InitializePuzzleCount();

	if (WinLoseSubsystem)
	{
		WinLoseSubsystem->OnWin.AddDynamic(this, &UML_AmbienceSubsystem::HandlePuzzleWon);
	}

	// Deferred by one tick: AML_BoardSpawner::BeginPlay (which restores bIsPuzzleSolved from the
	// save) hasn't necessarily run yet at this point, since world subsystems begin play before
	// actors do. Waiting one tick guarantees every board's solved state is settled before we read it.
	InWorld.GetTimerManager().SetTimer(
		SeedStateTimerHandle,
		this,
		&UML_AmbienceSubsystem::SeedStateFromAlreadySolvedBoards,
		KINDA_SMALL_NUMBER,
		false);
}

void UML_AmbienceSubsystem::Deinitialize()
{
	StopAmbience();
	StopMusicProgression();

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(SeedStateTimerHandle);
	}

	if (WinLoseSubsystem)
	{
		WinLoseSubsystem->OnWin.RemoveDynamic(this, &UML_AmbienceSubsystem::HandlePuzzleWon);
	}

	WonBoards.Reset();
	WinLoseSubsystem = nullptr;
	DevSettings = nullptr;

	Super::Deinitialize();
}

void UML_AmbienceSubsystem::StartAmbience()
{
	if (bAmbienceRunning)
	{
		return;
	}

	if (!DevSettings)
	{
		DevSettings = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();
	}

	if (!DevSettings || (DevSettings->LivingAmbienceEventPaths.IsEmpty() && DevSettings->DeadAmbienceEventPaths.IsEmpty()))
	{
		UE_LOG(LogMycelandAmbience, Warning, TEXT("Ambience Enviro has no FMOD event paths configured."));
		return;
	}

	bAmbienceRunning = true;
	PlayNextAmbienceSound();
}

void UML_AmbienceSubsystem::StopAmbience()
{
	bAmbienceRunning = false;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(AmbienceTimerHandle);
	}
}

float UML_AmbienceSubsystem::GetLivingAmbienceRatio() const
{
	if (TotalPuzzleCount <= 0)
	{
		return 0.0f;
	}

	return FMath::Clamp(static_cast<float>(WonPuzzleCount) / static_cast<float>(TotalPuzzleCount), 0.0f, 1.0f);
}

void UML_AmbienceSubsystem::HandlePuzzleWon()
{
	if (!WinLoseSubsystem)
	{
		return;
	}

	AML_BoardSpawner* WonBoard = WinLoseSubsystem->CurrentBoardSpawner;
	if (!IsValid(WonBoard))
	{
		return;
	}

	const FObjectKey WonBoardKey(WonBoard);
	if (WonBoards.Contains(WonBoardKey))
	{
		return;
	}

	WonBoards.Add(WonBoardKey);
	WonPuzzleCount = FMath::Clamp(WonPuzzleCount + 1, 0, FMath::Max(TotalPuzzleCount, 1));

	UE_LOG(LogMycelandAmbience, Log, TEXT("Ambience Enviro puzzle won: %d/%d (living ratio %.2f)"),
		WonPuzzleCount,
		TotalPuzzleCount,
		GetLivingAmbienceRatio());

	SwitchToMusicTrackForCurrentProgress();

	// ---------- Ancienne logique (layers additifs) — conservée en commentaire pour référence ----------
	// UnlockNextMusicLayer();
}

void UML_AmbienceSubsystem::PlayNextAmbienceSound()
{
	if (!bAmbienceRunning || !DevSettings)
	{
		return;
	}

	const TArray<FString>& LivingEvents = DevSettings->LivingAmbienceEventPaths;
	const TArray<FString>& DeadEvents = DevSettings->DeadAmbienceEventPaths;

	const bool bCanPlayLiving = !LivingEvents.IsEmpty();
	const bool bCanPlayDead = !DeadEvents.IsEmpty();
	if (!bCanPlayLiving && !bCanPlayDead)
	{
		StopAmbience();
		return;
	}

	const EML_LevelAmbienceMode AmbienceMode =
		GetAmbienceModeForCurrentLevel();

	if (AmbienceMode == EML_LevelAmbienceMode::None)
	{
		StopAmbience();
		return;
	}

	bool bPickLiving = false;

	switch (AmbienceMode)
	{
	case EML_LevelAmbienceMode::DeadOnly:
		bPickLiving = false;
		break;

	case EML_LevelAmbienceMode::LivingOnly:
		bPickLiving = true;
		break;

	case EML_LevelAmbienceMode::Normal:
	default:
		{
			const float LivingRatio = GetLivingAmbienceRatio();

			bPickLiving =
				bCanPlayLiving &&
				(!bCanPlayDead || FMath::FRand() < LivingRatio);

			break;
		}
	}
	const TArray<FString>& EventPool = bPickLiving ? LivingEvents : DeadEvents;
	const FString& EventPath = EventPool[FMath::RandRange(0, EventPool.Num() - 1)];

	if (UML_SoundSubsystem* SoundSubsystem = UML_SoundSubsystem::Get(this))
	{
		SoundSubsystem->StartSound2DByPath(EventPath);
		UE_LOG(LogMycelandAmbience, Verbose, TEXT("Played %s ambience event: %s"),
			bPickLiving ? TEXT("living") : TEXT("dead"),
			*EventPath);
	}

	ScheduleNextAmbienceSound();
}

void UML_AmbienceSubsystem::ScheduleNextAmbienceSound()
{
	UWorld* World = GetWorld();
	if (!World || !DevSettings || !bAmbienceRunning)
	{
		return;
	}

	const float MinDelay = FMath::Max(0.0f, DevSettings->AmbienceMinDelay);
	const float MaxDelay = FMath::Max(MinDelay, DevSettings->AmbienceMaxDelay);
	const float Delay = FMath::FRandRange(MinDelay, MaxDelay);

	World->GetTimerManager().SetTimer(
		AmbienceTimerHandle,
		this,
		&UML_AmbienceSubsystem::PlayNextAmbienceSound,
		Delay,
		false);
}

void UML_AmbienceSubsystem::InitializePuzzleCount()
{
	WonBoards.Reset();
	WonPuzzleCount = 0;
	TotalPuzzleCount = GetConfiguredPuzzleCountForCurrentLevel();

	if (TotalPuzzleCount <= 0 && DevSettings && DevSettings->bFallbackToBoardSpawnerCount)
	{
		TArray<AActor*> FoundBoards;
		UGameplayStatics::GetAllActorsOfClass(this, AML_BoardSpawner::StaticClass(), FoundBoards);

		// World boards are terrain, never a puzzle to win: they must not dilute the progression.
		TotalPuzzleCount = 0;
		for (const AActor* Actor : FoundBoards)
		{
			const AML_BoardSpawner* Board = Cast<AML_BoardSpawner>(Actor);
			if (IsValid(Board) && !Board->IsWorldBoard())
				++TotalPuzzleCount;
		}
	}

	TotalPuzzleCount = FMath::Max(TotalPuzzleCount, 1);

	UE_LOG(LogMycelandAmbience, Log, TEXT("Ambience Enviro initialized for %s with %d puzzle(s)."),
		*GetCleanMapName(),
		TotalPuzzleCount);
}

void UML_AmbienceSubsystem::SeedStateFromAlreadySolvedBoards()
{
	TArray<AActor*> FoundBoards;
	UGameplayStatics::GetAllActorsOfClass(this, AML_BoardSpawner::StaticClass(), FoundBoards);

	for (AActor* Actor : FoundBoards)
	{
		const AML_BoardSpawner* Board = Cast<AML_BoardSpawner>(Actor);
		if (IsValid(Board) && !Board->IsWorldBoard() && Board->bIsPuzzleSolved)
		{
			WonBoards.Add(FObjectKey(Board));
		}
	}

	WonPuzzleCount = FMath::Clamp(WonBoards.Num(), 0, TotalPuzzleCount);

	UE_LOG(LogMycelandAmbience, Log, TEXT("Ambience Enviro seeded from existing save state: %d/%d already solved."),
		WonPuzzleCount,
		TotalPuzzleCount);

	if (DevSettings && DevSettings->bAutoStartAmbienceEnviro)
	{
		StartAmbience();
	}

	if (DevSettings && DevSettings->bAutoStartMusicProgression)
	{
		StartMusicProgression();
	}

	// ---------- Ancienne logique (layers additifs) — conservée en commentaire pour référence ----------
	// if (DevSettings && DevSettings->bAutoStartMusicLayers)
	// {
	// 	StartMusicLayers();
	// }
}

void UML_AmbienceSubsystem::StartMusicProgression()
{
	SwitchToMusicTrackForCurrentProgress();
}
void UML_AmbienceSubsystem::StartPendingMusicTrack()
{
	if (PendingMusicEventPath.IsEmpty())
	{
		return;
	}

	UML_SoundSubsystem* SoundSubsystem = UML_SoundSubsystem::Get(this);
	if (!SoundSubsystem)
	{
		return;
	}

	const FString EventPath = PendingMusicEventPath;
	const int32 TrackIndex = PendingMusicTrackIndex;

	PendingMusicEventPath.Empty();
	PendingMusicTrackIndex = INDEX_NONE;

	CurrentMusicHandle =
		SoundSubsystem->StartTrackedSound2DByPath(
			EventPath,
			FML_OnSoundFinished(),
			/*bAutoDestroy=*/false
		);

	CurrentMusicTrackIndex = TrackIndex;

	UE_LOG(
		LogMycelandAmbience,
		Log,
		TEXT("Music progression started track %d after fade: %s"),
		TrackIndex,
		*EventPath
	);
}
void UML_AmbienceSubsystem::StopMusicProgression()
{
	if (IsValid(CurrentMusicHandle))
	{
		CurrentMusicHandle->Stop();
	}

	CurrentMusicHandle = nullptr;
	CurrentMusicTrackIndex = INDEX_NONE;
}

void UML_AmbienceSubsystem::SwitchToMusicTrackForCurrentProgress()
{
	if (!DevSettings)
	{
		DevSettings = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();
	}

	if (!DevSettings)
	{
		return;
	}

	UML_SoundSubsystem* SoundSubsystem = UML_SoundSubsystem::Get(this);
	if (!SoundSubsystem)
	{
		return;
	}

	// ============================================================
	// FIXED MUSIC LEVEL
	// ============================================================

	FString FixedMusicEventPath;

	if (GetFixedMusicForCurrentLevel(FixedMusicEventPath))
	{
		// INDEX_NONE - 1 is used internally to mean:
		// "fixed level music is currently playing".
		constexpr int32 FixedMusicIndex = INDEX_NONE - 1;

		// Puzzle wins still call this function, but fixed music must
		// never restart when that happens.
		if (CurrentMusicTrackIndex == FixedMusicIndex && IsValid(CurrentMusicHandle))
		{
			return;
		}

		if (IsValid(CurrentMusicHandle))
		{
			CurrentMusicHandle->Stop();
			CurrentMusicHandle = nullptr;
		}

		CurrentMusicHandle =
			SoundSubsystem->StartTrackedSound2DByPath(
				FixedMusicEventPath,
				FML_OnSoundFinished(),
				/*bAutoDestroy=*/false
			);

		CurrentMusicTrackIndex = FixedMusicIndex;

		UE_LOG(
			LogMycelandAmbience,
			Log,
			TEXT("Fixed level music started: %s"),
			*FixedMusicEventPath
		);

		return;
	}

	// ============================================================
	// NORMAL PUZZLE MUSIC PROGRESSION
	// ============================================================

	if (DevSettings->MusicTrackEventPaths.IsEmpty())
	{
		UE_LOG(
			LogMycelandAmbience,
			Warning,
			TEXT("Music progression has no FMOD event paths configured.")
		);

		return;
	}

	const int32 TargetTrackIndex =
		FMath::Clamp(
			WonPuzzleCount,
			0,
			DevSettings->MusicTrackEventPaths.Num() - 1
		);

	const bool bHasReachedLastTrack =
        TargetTrackIndex == DevSettings->MusicTrackEventPaths.Num() - 1;
    
    if (TargetTrackIndex == CurrentMusicTrackIndex &&
        IsValid(CurrentMusicHandle) &&
        !bHasReachedLastTrack)
    {
        return;
    }

	const FString& EventPath =
		DevSettings->MusicTrackEventPaths[TargetTrackIndex];

	// Something is already playing:
	// remember the new track and fade the current one out first.
	if (IsValid(CurrentMusicHandle))
	{
		PendingMusicEventPath = EventPath;
		PendingMusicTrackIndex = TargetTrackIndex;

		FadeOutMusic(MusicSwitchFadeDuration);
		return;
	}

	// Nothing currently playing, start immediately.
	PendingMusicEventPath = EventPath;
	PendingMusicTrackIndex = TargetTrackIndex;
	StartPendingMusicTrack();

	UE_LOG(
		LogMycelandAmbience,
		Log,
		TEXT("Music progression switched to track %d: %s"),
		TargetTrackIndex,
		*EventPath
	);
}

// ==================================================================================
// Ancienne logique (layers additifs) — conservée en commentaire pour référence.
// Elle démarrait un stem de plus par puzzle gagné et les empilait, au lieu de switcher
// d'une piste exclusive à l'autre comme le fait SwitchToMusicTrackForCurrentProgress ci-dessus.
// ==================================================================================
//
// void UML_AmbienceSubsystem::StartMusicLayers()
// {
// 	if (!ActiveMusicLayerHandles.IsEmpty())
// 	{
// 		return;
// 	}
//
// 	if (!DevSettings)
// 	{
// 		DevSettings = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();
// 	}
//
// 	if (!DevSettings || DevSettings->MusicLayerEventPaths.IsEmpty())
// 	{
// 		UE_LOG(LogMycelandAmbience, Warning, TEXT("Music layering has no FMOD event paths configured."));
// 		return;
// 	}
//
// 	// Catch up to however many layers were already unlocked (e.g. on a reloaded save), then leave
// 	// the rest to unlock one by one as future puzzles are won.
// 	const int32 LayersToStart = FMath::Clamp(WonPuzzleCount + 1, 1, DevSettings->MusicLayerEventPaths.Num());
// 	for (int32 i = 0; i < LayersToStart; ++i)
// 	{
// 		UnlockNextMusicLayer();
// 	}
// }
//
// void UML_AmbienceSubsystem::StopMusicLayers()
// {
// 	for (const TObjectPtr<UML_SoundPlaybackHandle>& Handle : ActiveMusicLayerHandles)
// 	{
// 		if (IsValid(Handle))
// 		{
// 			Handle->Stop();
// 		}
// 	}
//
// 	ActiveMusicLayerHandles.Reset();
// }
//
// void UML_AmbienceSubsystem::UnlockNextMusicLayer()
// {
// 	if (!DevSettings)
// 	{
// 		return;
// 	}
//
// 	const int32 NextLayerIndex = ActiveMusicLayerHandles.Num();
// 	if (!DevSettings->MusicLayerEventPaths.IsValidIndex(NextLayerIndex))
// 	{
// 		return; // every configured layer is already playing
// 	}
//
// 	UML_SoundSubsystem* SoundSubsystem = UML_SoundSubsystem::Get(this);
// 	if (!SoundSubsystem)
// 	{
// 		return;
// 	}
//
// 	const FString& EventPath = DevSettings->MusicLayerEventPaths[NextLayerIndex];
// 	if (UML_SoundPlaybackHandle* Handle = SoundSubsystem->StartTrackedSound2DByPath(EventPath, FML_OnSoundFinished(), /*bAutoDestroy=*/false))
// 	{
// 		ActiveMusicLayerHandles.Add(Handle);
// 		UE_LOG(LogMycelandAmbience, Log, TEXT("Music layer %d unlocked: %s"), NextLayerIndex, *EventPath);
// 	}
// }

int32 UML_AmbienceSubsystem::GetConfiguredPuzzleCountForCurrentLevel() const
{
	if (!DevSettings)
	{
		return 0;
	}

	const FString CurrentMapName = GetCleanMapName();
	for (const FML_LevelAmbiencePuzzleCount& Entry : DevSettings->AmbiencePuzzleCounts)
	{
		if (!Entry.Level.IsValid())
		{
			continue;
		}

		const TSoftObjectPtr<UWorld>* LevelAsset = DevSettings->Levels.Find(Entry.Level);
		if (!LevelAsset)
		{
			continue;
		}

		const FSoftObjectPath LevelPath = LevelAsset->ToSoftObjectPath();
		if (LevelPath.GetAssetName() == CurrentMapName)
		{
			return Entry.PuzzleCount;
		}
	}

	return 0;
}
bool UML_AmbienceSubsystem::GetFixedMusicForCurrentLevel(FString& OutEventPath) const
{
	OutEventPath.Empty();

	if (!DevSettings)
	{
		return false;
	}

	const FString CurrentMapName = GetCleanMapName();

	for (const FML_LevelFixedMusic& Entry : DevSettings->FixedMusicLevels)
	{
		if (!Entry.Level.IsValid() || Entry.MusicEventPath.IsEmpty())
		{
			continue;
		}

		const TSoftObjectPtr<UWorld>* LevelAsset =
			DevSettings->Levels.Find(Entry.Level);

		if (!LevelAsset)
		{
			continue;
		}

		const FSoftObjectPath LevelPath =
			LevelAsset->ToSoftObjectPath();

		if (LevelPath.GetAssetName() == CurrentMapName)
		{
			OutEventPath = Entry.MusicEventPath;
			return true;
		}
	}

	return false;
}

FString UML_AmbienceSubsystem::GetCleanMapName() const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return FString();
	}

	FString MapName = World->GetMapName();
	if (MapName.StartsWith(TEXT("UEDPIE_")))
	{
		const int32 PrefixEnd = MapName.Find(TEXT("_"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 7);
		if (PrefixEnd != INDEX_NONE)
		{
			MapName = MapName.Mid(PrefixEnd + 1);
		}
	}
	return MapName;
}
void UML_AmbienceSubsystem::FadeOutMusic(float Duration)
{
	if (!IsValid(CurrentMusicHandle))
		return;

	UFMODAudioComponent* AudioComponent = CurrentMusicHandle->GetAudioComponent();
	if (!IsValid(AudioComponent))
		return;

	if (!GetWorld())
		return;

	MusicFadeDuration = FMath::Max(Duration, 0.01f);
	MusicFadeElapsed = 0.0f;

	MusicFadeStartVolume = 1.0f;

	GetWorld()->GetTimerManager().ClearTimer(MusicFadeTimerHandle);

	GetWorld()->GetTimerManager().SetTimer(
		MusicFadeTimerHandle,
		this,
		&UML_AmbienceSubsystem::UpdateMusicFade,
		0.02f,
		true
	);
}
void UML_AmbienceSubsystem::UpdateMusicFade()
{
	if (!IsValid(CurrentMusicHandle))
	{
		GetWorld()->GetTimerManager().ClearTimer(MusicFadeTimerHandle);
		return;
	}

	UFMODAudioComponent* AudioComponent = CurrentMusicHandle->GetAudioComponent();
	if (!IsValid(AudioComponent))
	{
		GetWorld()->GetTimerManager().ClearTimer(MusicFadeTimerHandle);
		return;
	}

	MusicFadeElapsed += 0.02f;

	const float Alpha =
		FMath::Clamp(MusicFadeElapsed / MusicFadeDuration, 0.0f, 1.0f);

	const float NewVolume =
		FMath::Lerp(MusicFadeStartVolume, 0.0f, Alpha);

	AudioComponent->SetVolume(NewVolume);

	if (Alpha >= 1.0f)
	{
		GetWorld()->GetTimerManager().ClearTimer(MusicFadeTimerHandle);

		CurrentMusicHandle->Stop();
		CurrentMusicHandle = nullptr;
		CurrentMusicTrackIndex = INDEX_NONE;

		StartPendingMusicTrack();
	}
}

EML_LevelAmbienceMode UML_AmbienceSubsystem::GetAmbienceModeForCurrentLevel() const
{
	if (!DevSettings)
	{
		return EML_LevelAmbienceMode::Normal;
	}

	const FString CurrentMapName = GetCleanMapName();

	for (const FML_LevelFixedAmbience& Entry : DevSettings->FixedAmbienceLevels)
	{
		if (!Entry.Level.IsValid())
		{
			continue;
		}

		const TSoftObjectPtr<UWorld>* LevelAsset =
			DevSettings->Levels.Find(Entry.Level);

		if (!LevelAsset)
		{
			continue;
		}

		const FSoftObjectPath LevelPath =
			LevelAsset->ToSoftObjectPath();

		if (LevelPath.GetAssetName() == CurrentMapName)
		{
			return Entry.Mode;
		}
	}

	return EML_LevelAmbienceMode::Normal;
}