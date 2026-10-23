// Copyright Myceland Team, All Rights Reserved.


#include "Tiles/ML_Tile.h"

#include "NavModifierComponent.h"
#include "NavAreas/NavArea_Null.h"
#include "NavAreas/NavArea_Default.h"
#include "Core/ML_TileTypeTraits.h"
#include "Developer Settings/ML_MycelandDeveloperSettings.h"
#include "Tiles/ML_TileBase.h"
#include "Tiles/TileBase/ML_TileDirt.h"
#include "Tiles/TileBase/ML_TileGrass.h"
#include "Tiles/TileBase/ML_TileObstacle.h"
#include "Tiles/TileBase/ML_TileParasite.h"
#include "Tiles/TileBase/ML_TileTree.h"
#include "Tiles/TileBase/ML_TileWater.h"


AML_Tile::AML_Tile()
{
	PrimaryActorTick.bCanEverTick = false;
	
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;
	
	TileChildActor = CreateDefaultSubobject<UChildActorComponent>(TEXT("TileChildActor"));
	TileChildActor->SetupAttachment(RootComponent);

	// Purely visual highlight — no collision. Cursor detection AND physical collision are both on
	// GroundBase (a ground-level hexagon, in the child actor).
	HighlightTileMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("TileMesh"));
	HighlightTileMesh->SetupAttachment(RootComponent);
	HighlightTileMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HighlightTileMesh->SetGenerateOverlapEvents(false);

	HexagonCollision = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HexagonCollision"));
	HexagonCollision->SetupAttachment(RootComponent);
	HexagonCollision->SetRelativeScale3D(FVector(0.9f, 0.9f, 0.9f));
	HexagonCollision->SetCollisionObjectType(ECC_WorldStatic);
	HexagonCollision->SetCollisionResponseToAllChannels(ECR_Ignore);
	HexagonCollision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	HexagonCollision->SetGenerateOverlapEvents(false);
	HexagonCollision->SetHiddenInGame(true);
	HexagonCollision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// HexagonCollision only blocks the pawn physically. Keep it out of navigation so the
	// NavModifier below always carves from its (tall) FailsafeExtent instead of this thin,
	// ground-level hexagon, which would sit below the elevated navmesh and carve nothing.
	HexagonCollision->SetCanEverAffectNavigation(false);

	// No owner primitive feeds navigation (see above), so the modifier uses FailsafeExtent:
	// a box centred on the tile actor. Starts as a no-op (Default) since tiles begin unblocked;
	// SetBlocked() flips it to NavArea_Null to carve the hole.
	NavBlocker = CreateDefaultSubobject<UNavModifierComponent>(TEXT("NavBlocker"));
	NavBlocker->FailsafeExtent = NavBlockerExtent;
	NavBlocker->AreaClass = UNavArea_Default::StaticClass();
}

void AML_Tile::NotifyTileChangedNative()
{
	OnTileChangedNative.Broadcast(this);
}

void AML_Tile::DestroyStaleChildActors()
{
	if (!TileChildActor) return;

	const AActor* CurrentChild = TileChildActor->GetChildActor();

	TArray<AActor*> Attached;
	GetAttachedActors(Attached);

	for (AActor* Actor : Attached)
	{
		if (!IsValid(Actor)) continue;
		if (!Actor->IsA(AML_TileBase::StaticClass())) continue;
		if (Actor == CurrentChild) continue;

		// Orphaned tile visual from a previous class or an editor duplication.
		Actor->Destroy();
	}
}

void AML_Tile::SetBlocked(bool bNewBlocked)
{
	bBlocked = bNewBlocked;

	if (HexagonCollision)
	{
		HexagonCollision->SetCollisionEnabled(
			bBlocked
			? ECollisionEnabled::QueryOnly
			: ECollisionEnabled::NoCollision
		);
	}

	if (NavBlocker)
	{
		// Pick up any editor-tuned extent, then carve a hole when blocked / no-op when not.
		NavBlocker->FailsafeExtent = NavBlockerExtent;
		NavBlocker->SetAreaClass(
			bBlocked
			? UNavArea_Null::StaticClass()
			: UNavArea_Default::StaticClass()
		);
	}
}

#if WITH_EDITOR
void AML_Tile::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (!PropertyChangedEvent.Property) return;

	const FName PropertyName = PropertyChangedEvent.Property->GetFName();

	const bool bTypeChanged =
		PropertyName == GET_MEMBER_NAME_CHECKED(AML_Tile, CurrentType);

	const bool bClassChanged =
		PropertyName == GET_MEMBER_NAME_CHECKED(AML_Tile, DirtClass) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(AML_Tile, GrassClass) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(AML_Tile, ParasiteClass) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(AML_Tile, WaterClass) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(AML_Tile, ObstacleClass) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(AML_Tile, TreeClass);

	if (bTypeChanged || bClassChanged)
	{
		// Obstacles frame the board and are off-limits to the player, so default them to non-interactable
		// (no hover glow, no gamepad selection, not walkable). Only re-derived when the TYPE itself changes:
		// picking a different tile Blueprint — or a board grid refresh, which also calls UpdateClassInEditor
		// via Initialize() — must not silently wipe a designer's per-tile override.
		if (bTypeChanged)
			bIsInteractable = (CurrentType != EML_TileType::Obstacle);

		UpdateClassInEditor(CurrentType);
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AML_Tile, NavBlockerExtent))
	{
		// Reapply the carve box (and refresh navigation) with the new extent.
		SetBlocked(bBlocked);
	}
}

void AML_Tile::UpdateClassInEditor(const EML_TileType NewTileType)
{
	CurrentType = NewTileType;
	TSubclassOf<AML_TileBase> TileBase;

	switch (CurrentType)
	{
		case EML_TileType::Dirt:		TileBase = DirtClass; break;
		case EML_TileType::Grass:		TileBase = GrassClass; break;
		case EML_TileType::Parasite:	TileBase = ParasiteClass; break;
		case EML_TileType::Water:		TileBase = WaterClass; break;
		case EML_TileType::WaterPath:	TileBase = WaterClass; break;
		case EML_TileType::Obstacle:	TileBase = ObstacleClass; break;
		case EML_TileType::Tree:		TileBase = TreeClass; break;
	}

	TileChildActor->SetChildActorClass(TileBase);
	DestroyStaleChildActors();
	SetBlocked(UML_TileTypeTraits::IsBlocking(NewTileType));
}
#endif


void AML_Tile::UpdateClassAtRuntime(const EML_TileType NewTileType, const TSubclassOf<AML_TileBase> NewClass)
{
	if (!NewClass) return;

	const EML_TileType OldType = CurrentType;
	if (OldType == NewTileType) return;

	CurrentType = NewTileType;

	bConsumedGrass = (OldType == EML_TileType::Grass && NewTileType == EML_TileType::Parasite);

	// The incoming child actor has not animated yet, whatever the outgoing one had reported.
	bParasiteReady = false;

	TileChildActor->SetChildActorClass(NewClass);

	// Only touch collision/navigation when the blocking state actually changes:
	// SetBlocked dirties the navmesh, which is expensive to do per tile per wave.
	if (UML_TileTypeTraits::IsBlocking(NewTileType) != bBlocked)
	{
		SetBlocked(!bBlocked);
	}

	OnTileTypeChanged(OldType, NewTileType);
	NotifyTileChangedNative();
}

void AML_Tile::UpdateClassAtRuntime_Silent(const EML_TileType NewTileType, const TSubclassOf<AML_TileBase> NewClass)
{
	if (!NewClass) return;

	const EML_TileType OldType = CurrentType;
	if (OldType == NewTileType) return;

	// IMPORTANT: do NOT recompute bConsumedGrass here.
	// Undo system will restore bConsumedGrass explicitly from its record.

	CurrentType = NewTileType;

	bParasiteReady = false;

	TileChildActor->SetChildActorClass(NewClass);

	if (UML_TileTypeTraits::IsBlocking(NewTileType) != bBlocked)
		SetBlocked(!bBlocked);

	// NO OnTileTypeChanged(OldType, NewTileType) in silent mode
}

void AML_Tile::RefreshHeightLevelFromLocation()
{
	HeightLevel = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings()->GetHeightLevelFromZ(GetActorLocation().Z);
}

float AML_Tile::GetHeightLevelError() const
{
	const UML_MycelandDeveloperSettings* Settings = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();
	const float Z = GetActorLocation().Z;
	return FMath::Abs(Z - Settings->GetZFromHeightLevel(Settings->GetHeightLevelFromZ(Z)));
}

void AML_Tile::NotifyParasiteReady()
{
	if (bParasiteReady) return;

	bParasiteReady = true;
	OnParasiteReady.Broadcast(this);
}

void AML_Tile::Initialize(UML_BiomeTileSet* InBiomeTileSet)
{
#if WITH_EDITOR
	UpdateClassInEditor(CurrentType);
#else
	TSubclassOf<AML_TileBase> RuntimeClass = nullptr;
	if (InBiomeTileSet)
		RuntimeClass = InBiomeTileSet->GetClassFromTileType(CurrentType);

	if (RuntimeClass)
		UpdateClassAtRuntime(CurrentType, RuntimeClass);
#endif
}
