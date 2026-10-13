// Copyright Myceland Team, All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ML_NavigationBridgeComponent.generated.h"

class AML_BoardSpawner;
class AML_PlayerCharacter;
class AML_PlayerController;
class AML_Tile;
class UNavigationPath;
class UPathFollowingComponent;

UCLASS(ClassGroup=(Myceland), meta=(BlueprintSpawnableComponent))
class MYCELAND_API UML_NavigationBridgeComponent : public UActorComponent
{
	GENERATED_BODY()

private:
	UPROPERTY(Transient)
	AML_PlayerController* OwningController = nullptr;

	UPROPERTY(Transient)
	AML_PlayerCharacter* PlayerCharacter = nullptr;

	UPROPERTY(Transient)
	FVector PendingFreeMovementTarget = FVector::ZeroVector;

	bool bHasFreeMovementTarget = false;
	bool bIsUsingNavMeshMovement = false;
	float NavMeshAcceptanceRadius = 50.f;

	bool IsCompleteNavMeshPath(const UNavigationPath* Path, const FVector& Destination) const;

	// ---------- Free-movement hold steering ----------

	// Cursor destination the cached path was built for, and time elapsed since that build.
	FVector HoldSteerLastDestination = FVector::ZeroVector;
	float HoldSteerRefreshTimer = 0.f;

	// Corners of the cached navmesh path, and the index of the corner currently steered toward.
	TArray<FVector> HoldSteerPathPoints;
	int32 HoldSteerPointIndex = 0;

	void RefreshHoldSteeringPath(const FVector& Destination);

	// The UPathFollowingComponent created (or reused) by UAIBlueprintHelperLibrary::SimpleMoveToLocation
	// on OwningController. Used by StopNavMeshMovement to cancel the active move request properly
	// instead of only zeroing the CharacterMovementComponent's velocity.
	UPathFollowingComponent* FindPathFollowingComponent() const;

public:
	void Initialize(AML_PlayerController* Controller, AML_PlayerCharacter* Character, float InNavMeshAcceptanceRadius);

	void StartNavMeshMovement(const FVector& WorldLocation);
	void StopNavMeshMovement();
	bool TickNavMeshMovement(float DeltaTime);

	/**
	 * Steering direction for the free-movement hold: drives the player toward Destination ALONG the
	 * navmesh (walking around obstacles) instead of in a straight line.
	 *
	 * Returns false when no navmesh path could be built at all — the caller should then fall back to a
	 * straight-line direction so the hold never freezes. Returns true with a zero OutDirection when the
	 * path is fully consumed (destination reached, or a partial path exhausted against an obstacle):
	 * the caller must NOT move in that case.
	 */
	bool GetNavSteeringDirection(const FVector& Destination, float DeltaTime, FVector& OutDirection);

	/** Drops the cached hold path. Called at both ends of every click cycle. */
	void ResetHoldSteering();

	bool IsUsingNavMeshMovement() const { return bIsUsingNavMeshMovement; }
	AML_Tile* PredictNavMeshEntryTile(const AML_BoardSpawner* Board, const FVector& Destination) const;
	AML_Tile* FindReachableExitBorderTile(const AML_BoardSpawner* Board, const FVector& OutsideDestination) const;
};
