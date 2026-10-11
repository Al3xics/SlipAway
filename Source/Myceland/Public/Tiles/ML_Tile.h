// Copyright Myceland Team, All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ML_BoardSpawner.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "Core/ML_CoreData.h"
#include "ML_Tile.generated.h"

class AML_TileTree;
class AML_TileBase;
class AML_TileObstacle;
class AML_TileWater;
class AML_TileParasite;
class AML_TileGrass;
class AML_TileDirt;
class AML_TileWaterPath;
class AML_BoardSpawner;
class UNavModifierComponent;
enum class EML_TileType : uint8;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnTileChangedNative, AML_Tile*, Tile);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnTileParasiteReady, AML_Tile*, Tile);

UCLASS(Blueprintable)
class MYCELAND_API AML_Tile : public AActor
{
	GENERATED_BODY()

private:
	UPROPERTY(EditInstanceOnly, Category="Myceland Tile")
	EML_TileType CurrentType = EML_TileType::Dirt;
	
	UPROPERTY(EditAnywhere, Category="Myceland Tile", meta=(EditCondition="CurrentType==EML_TileType::Dirt", EditConditionHides))
	TSubclassOf<AML_TileDirt> DirtClass;
	
	UPROPERTY(EditAnywhere, Category="Myceland Tile", meta=(EditCondition="CurrentType==EML_TileType::Grass", EditConditionHides))
	TSubclassOf<AML_TileGrass> GrassClass;
	
	UPROPERTY(EditAnywhere, Category="Myceland Tile", meta=(EditCondition="CurrentType==EML_TileType::Parasite", EditConditionHides))
	TSubclassOf<AML_TileParasite> ParasiteClass;
	
	UPROPERTY(EditAnywhere, Category="Myceland Tile", meta=(EditCondition="CurrentType==EML_TileType::Water", EditConditionHides))
	TSubclassOf<AML_TileWater> WaterClass;
	
	UPROPERTY(EditAnywhere, Category="Myceland Tile", meta=(EditCondition="CurrentType==EML_TileType::Obstacle", EditConditionHides))
	TSubclassOf<AML_TileObstacle> ObstacleClass;
	
	UPROPERTY(EditAnywhere, Category="Myceland Tile", meta=(EditCondition="CurrentType==EML_TileType::Tree", EditConditionHides))
	TSubclassOf<AML_TileTree> TreeClass;

	UPROPERTY(EditAnywhere, Category="Myceland Tile", meta=(EditCondition="CurrentType==EML_TileType::WaterPath", EditConditionHides))
	TSubclassOf<AML_TileWaterPath> WaterPathClass;
	
	UPROPERTY(VisibleAnywhere, Category="Myceland Tile")
	FIntPoint AxialCoord = FIntPoint(0, 0);

	UPROPERTY(VisibleAnywhere, Category="Myceland Tile")
	bool bBlocked = false;
	
	UPROPERTY(VisibleAnywhere, Category="Myceland Tile")
	bool bHasCollectible = false;

	// Reset on every type change: it describes the child actor currently on the tile, not the tile itself.
	bool bParasiteReady = false;
	
	UPROPERTY(VisibleAnywhere, Category="Myceland Tile")
	bool bIsBorderTile = false;

	// False on tiles that belong to the board but are off-limits to the player — typically the Obstacle
	// ring that frames a board and separates it from the environment. Such a tile never glows on hover
	// (mouse OR gamepad), can never be picked by the gamepad selection cursor, and is not walkable.
	// This gates PLAYER INTERACTION only, never puzzle logic: waves keep propagating through these tiles
	// (grass needs obstacles to travel around a lake — see UML_TileTypeTraits::CanGrassPropagateTo).
	// Auto-set to false when a tile is switched to Obstacle in the editor; override it per tile if an
	// obstacle must stay interactable.
	UPROPERTY(EditAnywhere, Category="Myceland Tile")
	bool bIsInteractable = true;

	// Height level of this tile on the global grid: its Z above the Hex Grid Origin, divided by Tile Height Step
	// (Project Settings > Myceland > Gameplay). Derived from the tile's location, never typed by hand: move the
	// tile up or down, then click "Snap Tile Heights" on its board. Movement compares the levels of two neighbor
	// tiles to know whether the player can step from one to the other (UML_MycelandDeveloperSettings::IsHeightStepAllowed).
	UPROPERTY(VisibleAnywhere, Category="Myceland Tile|Height")
	int32 HeightLevel = 0;

	// Coordinate on the global hex grid shared by every board (see UML_HexGridSubsystem), set when the board
	// registers its tiles at BeginPlay. Different from AxialCoord, which is local to the board and is what
	// puzzle logic and saves use. Movement uses this one.
	UPROPERTY(VisibleInstanceOnly, Transient, Category="Myceland Tile")
	FIntPoint GlobalAxialCoord = FIntPoint(0, 0);

	void SetBlocked(bool bNewBlocked);

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Myceland Tile")
	USceneComponent* SceneRoot;
	
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Myceland Tile")
	UChildActorComponent* TileChildActor;
	
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Myceland Tile")
	UStaticMeshComponent* HighlightTileMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Myceland Tile")
	UStaticMeshComponent* HexagonCollision;

	// Carves a hole in the navmesh when this tile blocks movement, so pathfinding routes
	// around non-walkable tiles (water, obstacle, tree, parasite) instead of through them.
	// Lives on the parent tile (not the child actor) and uses the owner's location.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Myceland Tile|Navigation")
	UNavModifierComponent* NavBlocker;

	// Half-extents of the navmesh carve box applied while blocked. Z must be tall enough to
	// reach the navmesh, which is generated above the tile ground. Tune per tile size / navmesh height.
	UPROPERTY(EditAnywhere, Category="Myceland Tile|Navigation")
	FVector NavBlockerExtent = FVector(55.f, 55.f, 500.f);

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	void UpdateClassInEditor(EML_TileType NewTileType);
#endif

public:
	bool bConsumedGrass = false;
	UPROPERTY(BlueprintAssignable, Category="Tile")
    FOnTileChangedNative OnTileChangedNative;
    
    UFUNCTION(BlueprintCallable, Category="Tile")
    void NotifyTileChangedNative();
	AML_Tile();

	UFUNCTION(BlueprintCallable, Category="Myceland Tile|Getter & Setter")
	void SetAxialCoord(const FIntPoint& InAxial) { AxialCoord = InAxial; }

	UFUNCTION(BlueprintPure, Category="Myceland Tile|Getter & Setter")
	FIntPoint GetAxialCoord() const { return AxialCoord; }

	// Coordinate on the global grid (see GlobalAxialCoord). Only meaningful once the board has registered.
	UFUNCTION(BlueprintPure, Category="Myceland Tile|Getter & Setter")
	FIntPoint GetGlobalAxialCoord() const { return GlobalAxialCoord; }

	void SetGlobalAxialCoord(const FIntPoint& InGlobalAxial) { GlobalAxialCoord = InGlobalAxial; }

	UFUNCTION(BlueprintCallable, Category="Myceland Tile|Getter & Setter")
	void SetCurrentType(const EML_TileType NewType) { CurrentType = NewType; }

	UFUNCTION(BlueprintPure, Category="Myceland Tile|Getter & Setter")
	EML_TileType GetCurrentType() const { return CurrentType; }

	UFUNCTION(BlueprintPure, Category="Myceland Tile|Getter & Setter")
	bool IsBlocked() const { return bBlocked; }

	// See bIsInteractable: false means no hover glow, no gamepad selection, not walkable.
	UFUNCTION(BlueprintPure, Category="Myceland Tile|Getter & Setter")
	bool IsInteractable() const { return bIsInteractable; }

	UFUNCTION(BlueprintCallable, Category="Myceland Tile|Getter & Setter")
	void SetInteractable(const bool bNewValue) { bIsInteractable = bNewValue; }

	UFUNCTION(BlueprintCallable, Category="Myceland Tile|Getter & Setter")
	void SetBorderTile(const bool Value) { bIsBorderTile = Value; }

	UFUNCTION(BlueprintPure, Category="Myceland Tile|Height")
	int32 GetHeightLevel() const { return HeightLevel; }

	// Recomputes HeightLevel from the tile's current Z. The board runs it on every grid refresh.
	void RefreshHeightLevelFromLocation();

	// Distance (cm) between the tile's Z and the exact Z of its level. Anything but ~0 means the tile
	// sits between two levels and needs a "Snap Tile Heights" on its board.
	float GetHeightLevelError() const;

	UFUNCTION(BlueprintPure, Category="Myceland Tile|Getter & Setter")
	bool IsBorderTile() const { return bIsBorderTile; }

	UFUNCTION(BlueprintCallable, Category="Myceland Tile|Collectible")
	void SetHasCollectible(const bool bNewValue) { bHasCollectible = bNewValue; }
	
	UFUNCTION(BlueprintPure, Category="Myceland Tile|Collectible")
	bool HasCollectible() const { return bHasCollectible; }
	
	UFUNCTION(BlueprintPure, Category="Myceland Tile|Getter & Setter")
	AML_BoardSpawner* GetBoardSpawnerFromTile() const { return Cast<AML_BoardSpawner>(GetOwner()); }

	// Single entry point for the cursor / selection glow: the state fully describes what must be shown,
	// so the Blueprint only maps a state to a color. It must be TOTAL: every state, None included (which
	// hides the glow), has to leave the tile in a consistent visual state, because the same tile can be
	// pushed a new state without ever being "unhovered" first (the player leaves it, its type changes,
	// the energy runs out). Nothing else may write the hover color or destroy the hover VFX:
	// UML_HoverPreviewComponent owns that visual and only pushes it when the state actually changes.
	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category="Myceland Tile|Feedback")
	void SetCursorHoverState(EML_TileHoverState State);

	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category="Myceland Tile|Feedback")
	void GlowPathWalk();

	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category="Myceland Tile|Feedback")
	void StopGlowingPathWalk();

	// Gamepad: glow used to advertise a tile the player can plant on (a plantable neighbor around
	// the player). Distinct from SetCursorHoverState, which marks the single currently-selected tile.
	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category="Myceland Tile|Feedback")
	void GlowPlantableAvailable();

	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category="Myceland Tile|Feedback")
	void StopGlowingPlantableAvailable();
	
	UFUNCTION(BlueprintImplementableEvent, Category="Myceland Tile|Feedback")
	void OnTileTypeChanged(EML_TileType OldType, EML_TileType NewType);

	UFUNCTION(BlueprintImplementableEvent, Category="Myceland Tile|Feedback")
	void OnWaveTouched();
	
	
	UFUNCTION()
	void UpdateClassAtRuntime(const EML_TileType NewTileType, TSubclassOf<AML_TileBase> NewClass);
	
	UFUNCTION()
	void Initialize(UML_BiomeTileSet* InBiomeTileSet);
	
	UFUNCTION()
	UChildActorComponent* GetTileChildActor() const { return TileChildActor; }

	UFUNCTION()
	void UpdateClassAtRuntime_Silent(EML_TileType NewTileType, TSubclassOf<AML_TileBase> NewClass);

	// Destroys any AML_TileBase actor attached to this tile that is NOT the child actor
	// currently tracked by TileChildActor. Guards against orphaned child actors left behind
	// by editor duplication / reattachment, which otherwise keep showing the previous type.
	void DestroyStaleChildActors();

	UPROPERTY(Transient)
	TWeakObjectPtr<AML_Collectible> CollectibleActor;

	// A tile only *becomes* Parasite after GrassToParasiteDelay, and the parasite Blueprint then plays its
	// growth animation on top of that. Anything that must appear to come out of a finished parasite (the
	// collectible flight) waits on this instead of guessing a duration.
	UPROPERTY(BlueprintAssignable, Category="Myceland Tile|Parasite")
	FOnTileParasiteReady OnParasiteReady;

	UFUNCTION(BlueprintPure, Category="Myceland Tile|Parasite")
	bool IsParasiteReady() const { return bParasiteReady; }

	// Called by the parasite Blueprint at the end of its transformation animation. Idempotent, so it can be
	// wired on several timelines without ordering the branches.
	UFUNCTION(BlueprintCallable, Category="Myceland Tile|Parasite")
	void NotifyParasiteReady();
};
