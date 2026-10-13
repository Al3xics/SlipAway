// Copyright Myceland Team, All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tiles/ML_BoardSpawner.h"
#include "ML_HexGridSubsystem.generated.h"

class AML_Tile;

/**
 * One hex grid for the whole world: every board (puzzle, hub, world) registers its tiles here, keyed by their
 * GLOBAL axial coordinate on the shared lattice (Project Settings > Myceland > Gameplay > Hex Grid).
 *
 * Movement goes through this grid instead of a single board's GridMap, which is what lets the player walk from
 * one board to the next. A tile's board-local AxialCoord is untouched: puzzle logic, saves and waves keep using it.
 *
 * Boards register in BeginPlay and unregister in EndPlay, so streamed-in / streamed-out boards join and leave the
 * grid on their own. Two tiles landing on the same global cell (overlapping boards) is an authoring mistake: the
 * first one registered wins and the board that lost cells logs a warning.
 *
 * Debug: console command "ml.HexGrid.Debug [Seconds]" draws each tile's global coordinate and height level, and
 * links neighbor tiles in green (both ways walkable by height), yellow (one way only: a drop) or red (neither).
 */
UCLASS()
class MYCELAND_API UML_HexGridSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	static UML_HexGridSubsystem* Get(const UObject* WorldContextObject);

	// ==================== Registration ====================

	// Adds every tile of Board to the grid. Safe to call again: the board's previous cells are dropped first.
	void RegisterBoard(AML_BoardSpawner* Board);

	// Removes every cell this board registered.
	void UnregisterBoard(const AML_BoardSpawner* Board);

	// Fired after any board joins or leaves the grid, for caches built on top of it (pathfinding, hover).
	DECLARE_MULTICAST_DELEGATE(FOnHexGridChanged);
	FOnHexGridChanged OnGridChanged;

	// ==================== Lattice Math ====================

	// Global axial cell containing WorldLocation (Z ignored).
	static FIntPoint WorldToGlobalAxial(const FVector& WorldLocation);

	// World center of a global cell at a given height level.
	static FVector GlobalAxialToWorld(const FIntPoint& GlobalAxial, int32 HeightLevel);

	// ==================== Queries ====================

	UFUNCTION(BlueprintPure, Category="Myceland Hex Grid")
	AML_Tile* GetTileAtGlobalAxial(FIntPoint GlobalAxial) const;

	// Tile whose cell contains WorldLocation (Z ignored), or nullptr.
	UFUNCTION(BlueprintPure, Category="Myceland Hex Grid")
	AML_Tile* GetTileAtWorldLocation(const FVector& WorldLocation) const;

	// Fills the caller's inline buffer with the 6 neighbors, in Directions order, nullptr where there is no tile.
	// Neighbors can belong to another board. No allocation.
	void GetNeighbors(const AML_Tile* Tile, FML_TileNeighbors& OutNeighbors) const;

	// Blueprint copy of the above (UHT can't expose the inline-allocator array).
	UFUNCTION(BlueprintPure, Category="Myceland Hex Grid", meta=(DisplayName="Get Neighbors"))
	TArray<AML_Tile*> GetNeighbors_BP(AML_Tile* Tile) const;

	UFUNCTION(BlueprintPure, Category="Myceland Hex Grid")
	int32 GetRegisteredTileCount() const { return Cells.Num(); }

	// ==================== Debug ====================

	// See the class comment. Compiles to nothing in shipping.
	void DebugDraw(float Duration) const;

private:
	TMap<FIntPoint, TWeakObjectPtr<AML_Tile>> Cells;

	// Which cells each board owns, so unregistering never has to scan the whole grid.
	TMap<TWeakObjectPtr<const AML_BoardSpawner>, TArray<FIntPoint>> BoardCells;
};
