// Copyright Myceland Team, All Rights Reserved.

#include "Subsystem/ML_HexGridSubsystem.h"

#include "Core/ML_CoreData.h"
#include "Developer Settings/ML_MycelandDeveloperSettings.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Tiles/ML_BoardSpawner.h"
#include "Tiles/ML_Tile.h"

UML_HexGridSubsystem* UML_HexGridSubsystem::Get(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	return World ? World->GetSubsystem<UML_HexGridSubsystem>() : nullptr;
}

// ==================== Registration ====================

void UML_HexGridSubsystem::RegisterBoard(AML_BoardSpawner* Board)
{
	if (!IsValid(Board)) return;

	UnregisterBoard(Board);

	TArray<FIntPoint>& Owned = BoardCells.Add(Board);
	int32 Collisions = 0;

	for (AML_Tile* Tile : Board->GetGridTiles())
	{
		if (!IsValid(Tile)) continue;

		const FIntPoint GlobalAxial = WorldToGlobalAxial(Tile->GetActorLocation());

		if (const TWeakObjectPtr<AML_Tile>* Existing = Cells.Find(GlobalAxial); Existing && Existing->IsValid())
		{
			++Collisions;
			continue;
		}

		Cells.Add(GlobalAxial, Tile);
		Owned.Add(GlobalAxial);
		Tile->SetGlobalAxialCoord(GlobalAxial);
	}

#if !UE_BUILD_SHIPPING
	if (Collisions > 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[HexGrid] Board '%s': %d tile(s) land on a cell another board already occupies — they are left out of "
			     "the global grid, so the player can never step on them. Boards must not overlap."),
			*Board->GetName(), Collisions);
	}
#endif

	OnGridChanged.Broadcast();
}

void UML_HexGridSubsystem::UnregisterBoard(const AML_BoardSpawner* Board)
{
	TArray<FIntPoint> Owned;
	if (!BoardCells.RemoveAndCopyValue(Board, Owned))
		return;

	for (const FIntPoint& Cell : Owned)
		Cells.Remove(Cell);

	OnGridChanged.Broadcast();
}

// ==================== Lattice Math ====================

FIntPoint UML_HexGridSubsystem::WorldToGlobalAxial(const FVector& WorldLocation)
{
	const FVector& Origin = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings()->HexGridOrigin;
	return AML_BoardSpawner::Local2DToAxial(FVector2D(WorldLocation - Origin));
}

FVector UML_HexGridSubsystem::GlobalAxialToWorld(const FIntPoint& GlobalAxial, const int32 HeightLevel)
{
	const UML_MycelandDeveloperSettings* Settings = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();
	const FVector2D Local = AML_BoardSpawner::AxialToLocal2D(GlobalAxial.X, GlobalAxial.Y);
	return FVector(Settings->HexGridOrigin.X + Local.X, Settings->HexGridOrigin.Y + Local.Y, Settings->GetZFromHeightLevel(HeightLevel));
}

// ==================== Queries ====================

AML_Tile* UML_HexGridSubsystem::GetTileAtGlobalAxial(const FIntPoint GlobalAxial) const
{
	const TWeakObjectPtr<AML_Tile>* Found = Cells.Find(GlobalAxial);
	return Found ? Found->Get() : nullptr;
}

AML_Tile* UML_HexGridSubsystem::GetTileAtWorldLocation(const FVector& WorldLocation) const
{
	return GetTileAtGlobalAxial(WorldToGlobalAxial(WorldLocation));
}

void UML_HexGridSubsystem::GetNeighbors(const AML_Tile* Tile, FML_TileNeighbors& OutNeighbors) const
{
	OutNeighbors.Reset();
	if (!IsValid(Tile)) return;

	const FIntPoint Center = Tile->GetGlobalAxialCoord();
	for (const FIntPoint& Dir : Directions)
		OutNeighbors.Add(GetTileAtGlobalAxial(Center + Dir));
}

TArray<AML_Tile*> UML_HexGridSubsystem::GetNeighbors_BP(AML_Tile* Tile) const
{
	FML_TileNeighbors Neighbors;
	GetNeighbors(Tile, Neighbors);
	return TArray<AML_Tile*>(Neighbors);
}

// ==================== Debug ====================

void UML_HexGridSubsystem::DebugDraw(const float Duration) const
{
#if ENABLE_DRAW_DEBUG
	const UWorld* World = GetWorld();
	if (!World) return;

	const UML_MycelandDeveloperSettings* Settings = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();
	constexpr float LabelHeight = 80.f;
	constexpr float LinkHeight = 30.f;

	for (const TPair<FIntPoint, TWeakObjectPtr<AML_Tile>>& Pair : Cells)
	{
		const AML_Tile* Tile = Pair.Value.Get();
		if (!Tile) continue;

		const FVector Center = Tile->GetActorLocation();
		// One stable hue per board, so you can see where a board ends.
		const FColor BoardColor = FLinearColor::MakeFromHSV8(static_cast<uint8>(GetTypeHash(Tile->GetOwner()) % 255), 160, 255).ToFColor(true);

		DrawDebugString(World, Center + FVector(0.f, 0.f, LabelHeight),
			FString::Printf(TEXT("%d,%d  L%d"), Pair.Key.X, Pair.Key.Y, Tile->GetHeightLevel()),
			nullptr, BoardColor, Duration, true);

		// Each pair once: only toward the first three directions.
		for (int32 DirIndex = 0; DirIndex < 3; ++DirIndex)
		{
			const AML_Tile* Neighbor = GetTileAtGlobalAxial(Pair.Key + Directions[DirIndex]);
			if (!Neighbor) continue;

			const bool bThere = Settings->IsHeightStepAllowed(Tile->GetHeightLevel(), Neighbor->GetHeightLevel());
			const bool bBack  = Settings->IsHeightStepAllowed(Neighbor->GetHeightLevel(), Tile->GetHeightLevel());
			const FColor LinkColor = (bThere && bBack) ? FColor::Green : (bThere || bBack) ? FColor::Yellow : FColor::Red;

			DrawDebugLine(World, Center + FVector(0.f, 0.f, LinkHeight), Neighbor->GetActorLocation() + FVector(0.f, 0.f, LinkHeight),
				LinkColor, false, Duration, 0, 3.f);
		}
	}
#endif
}

#if !UE_BUILD_SHIPPING
static FAutoConsoleCommandWithWorldAndArgs GML_HexGridDebugCommand(
	TEXT("ml.HexGrid.Debug"),
	TEXT("Draws the global hex grid for N seconds (default 10): global coordinate + height level per tile, and links "
	     "between neighbors — green: walkable both ways, yellow: one way (a drop), red: neither."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		const float Duration = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 10.f;
		if (const UML_HexGridSubsystem* Grid = UML_HexGridSubsystem::Get(World))
		{
			UE_LOG(LogTemp, Log, TEXT("[HexGrid] %d tile(s) registered."), Grid->GetRegisteredTileCount());
			Grid->DebugDraw(Duration > 0.f ? Duration : 10.f);
		}
	}));
#endif
