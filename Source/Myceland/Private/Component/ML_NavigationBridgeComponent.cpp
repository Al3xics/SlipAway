// Copyright Myceland Team, All Rights Reserved.

#include "Component/ML_NavigationBridgeComponent.h"

#include "Blueprint/AIBlueprintHelperLibrary.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "Navigation/PathFollowingComponent.h"
#include "Player/ML_HexPathfinder.h"
#include "Player/ML_PlayerCharacter.h"
#include "Player/ML_PlayerController.h"
#include "Tiles/ML_BoardSpawner.h"
#include "Tiles/ML_Tile.h"

void UML_NavigationBridgeComponent::Initialize(AML_PlayerController* Controller, AML_PlayerCharacter* Character, float InNavMeshAcceptanceRadius)
{
	OwningController = Controller;
	PlayerCharacter = Character;
	NavMeshAcceptanceRadius = InNavMeshAcceptanceRadius;
}

void UML_NavigationBridgeComponent::StartNavMeshMovement(const FVector& WorldLocation)
{
	if (!IsValid(OwningController) || !IsValid(PlayerCharacter))
		return;

	UAIBlueprintHelperLibrary::SimpleMoveToLocation(OwningController, WorldLocation);

	PendingFreeMovementTarget = WorldLocation;
	bHasFreeMovementTarget = true;
	bIsUsingNavMeshMovement = true;
}

void UML_NavigationBridgeComponent::StopNavMeshMovement()
{
	if (!bIsUsingNavMeshMovement)
		return;

	// Cancel the active AI move request (created by SimpleMoveToLocation in StartNavMeshMovement)
	// through the PathFollowingComponent itself, rather than only calling StopMovementImmediately
	// on the CharacterMovementComponent. StopMovementImmediately snaps velocity to zero instantly,
	// which made the AnimBP's locomotion blend (driven by Velocity.Size()) flash to Idle for a frame
	// every time a new click redirected movement while the character was already walking. AbortMove
	// lets the character brake at its normal deceleration instead, and it also makes sure the
	// PathFollowingComponent stops issuing move requests toward the old destination — previously it
	// kept ticking toward the stale goal for a frame or two, fighting the new input/path request.
	if (UPathFollowingComponent* PFollowComp = FindPathFollowingComponent())
	{
		// No explicit status check: AbortMove is a safe no-op when there's no active request.
		// AbortFlags has no default in this engine version, so it must be passed explicitly.
		PFollowComp->AbortMove(*OwningController, FPathFollowingResultFlags::UserAbort);
	}
	else if (IsValid(PlayerCharacter))
	{
		// Defensive fallback: should not normally happen since StartNavMeshMovement always creates
		// one via SimpleMoveToLocation, but avoids leaving the character mid-slide if it's ever missing.
		if (UCharacterMovementComponent* MovementComponent = PlayerCharacter->GetCharacterMovement())
		{
			MovementComponent->StopMovementImmediately();
		}
	}

	bIsUsingNavMeshMovement = false;
	bHasFreeMovementTarget = false;
}

void UML_NavigationBridgeComponent::ResetHoldSteering()
{
	HoldSteerPathPoints.Reset();
	HoldSteerPointIndex = 0;
	HoldSteerRefreshTimer = 0.f;
	HoldSteerLastDestination = FVector::ZeroVector;
}

void UML_NavigationBridgeComponent::RefreshHoldSteeringPath(const FVector& Destination)
{
	HoldSteerRefreshTimer = 0.f;
	HoldSteerLastDestination = Destination;
	HoldSteerPathPoints.Reset();
	HoldSteerPointIndex = 0;

	if (!IsValid(PlayerCharacter))
		return;

	UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(
		GetWorld(),
		PlayerCharacter->GetActorLocation(),
		Destination);

	// Partial paths are kept on purpose (unlike IsCompleteNavMeshPath, which rejects them): when the
	// cursor sits on an obstacle or off the navmesh, the player should still walk as close as the
	// navmesh allows rather than freeze in place the moment the cursor crosses onto an obstacle.
	if (!Path || !Path->IsValid() || Path->PathPoints.Num() == 0)
		return;

	HoldSteerPathPoints = Path->PathPoints;
	// PathPoints[0] is the character's own position: always steer toward a corner ahead of it.
	HoldSteerPointIndex = HoldSteerPathPoints.Num() > 1 ? 1 : 0;
}

bool UML_NavigationBridgeComponent::GetNavSteeringDirection(const FVector& Destination, float DeltaTime, FVector& OutDirection)
{
	OutDirection = FVector::ZeroVector;

	if (!IsValid(PlayerCharacter))
		return false;

	// Rebuilding the path every frame would run one synchronous query per tick for nothing: the corners
	// only change when the cursor moves meaningfully. So refresh on a timer, plus immediately whenever
	// the cursor jumps, so a big cursor move still redirects the player on the very next frame.
	constexpr float RefreshInterval = 0.1f;
	constexpr float DestinationTolerance = 100.f;

	// Tighter than NavMeshAcceptanceRadius on purpose: a corner only counts as passed once the player
	// is nearly on it, otherwise we would start aiming at the corner AFTER it and cut straight across
	// the geometry the navmesh was routing us around.
	constexpr float CornerReachedRadius = 25.f;

	HoldSteerRefreshTimer += DeltaTime;

	const bool bDestinationMoved =
		FVector::DistSquared2D(Destination, HoldSteerLastDestination) > FMath::Square(DestinationTolerance);

	if (HoldSteerPathPoints.Num() == 0 || bDestinationMoved || HoldSteerRefreshTimer >= RefreshInterval)
		RefreshHoldSteeringPath(Destination);

	if (HoldSteerPathPoints.Num() == 0)
		return false;

	// Skip the corners already reached — a tight zig-zag can cross several within one refresh window.
	const FVector CurrentLocation = PlayerCharacter->GetActorLocation();
	while (HoldSteerPathPoints.IsValidIndex(HoldSteerPointIndex) &&
	       FVector::DistSquared2D(CurrentLocation, HoldSteerPathPoints[HoldSteerPointIndex]) <= FMath::Square(CornerReachedRadius))
	{
		++HoldSteerPointIndex;
	}

	// Path fully consumed: destination reached, or a partial path ran out against an obstacle.
	// Zero direction with a true return means "stay put", so the caller never pushes into the obstacle.
	if (!HoldSteerPathPoints.IsValidIndex(HoldSteerPointIndex))
		return true;

	OutDirection = (HoldSteerPathPoints[HoldSteerPointIndex] - CurrentLocation).GetSafeNormal2D();
	return true;
}

UPathFollowingComponent* UML_NavigationBridgeComponent::FindPathFollowingComponent() const
{
	return IsValid(OwningController) ? OwningController->FindComponentByClass<UPathFollowingComponent>() : nullptr;
}

bool UML_NavigationBridgeComponent::TickNavMeshMovement(float DeltaTime)
{
	if (!bIsUsingNavMeshMovement || !bHasFreeMovementTarget)
		return false;

	if (!IsValid(PlayerCharacter))
	{
		StopNavMeshMovement();
		return false;
	}

	const FVector CurrentLocation = PlayerCharacter->GetActorLocation();
	const float DistSq = FVector::DistSquared2D(CurrentLocation, PendingFreeMovementTarget);

	if (DistSq > FMath::Square(NavMeshAcceptanceRadius))
		return false;

	StopNavMeshMovement();
	return true;
}

bool UML_NavigationBridgeComponent::IsCompleteNavMeshPath(const UNavigationPath* Path, const FVector& Destination) const
{
	if (!Path || !Path->IsValid() || Path->PathPoints.Num() == 0 || Path->IsPartial())
		return false;

	const FVector LastPoint = Path->PathPoints.Last();
	return FVector::DistSquared2D(LastPoint, Destination) <= FMath::Square(NavMeshAcceptanceRadius);
}

AML_Tile* UML_NavigationBridgeComponent::PredictNavMeshEntryTile(const AML_BoardSpawner* Board, const FVector& Destination) const
{
	if (!IsValid(Board) || !IsValid(PlayerCharacter))
		return nullptr;

	UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(
		GetWorld(),
		PlayerCharacter->GetActorLocation(),
		Destination);

	if (!IsCompleteNavMeshPath(Path, Destination))
		return nullptr;

	const TMap<FIntPoint, AML_Tile*> GridMap = Board->GetGridMap();
	constexpr float SampleSpacing = 50.f;

	for (int32 PointIndex = 1; PointIndex < Path->PathPoints.Num(); ++PointIndex)
	{
		const FVector SegmentStart = Path->PathPoints[PointIndex - 1];
		const FVector SegmentEnd = Path->PathPoints[PointIndex];
		const float SegmentLength = FVector::Dist2D(SegmentStart, SegmentEnd);
		const int32 StepCount = FMath::Max(1, FMath::CeilToInt(SegmentLength / SampleSpacing));

		for (int32 Step = 1; Step <= StepCount; ++Step)
		{
			const float Alpha = static_cast<float>(Step) / static_cast<float>(StepCount);
			const FVector SamplePoint = FMath::Lerp(SegmentStart, SegmentEnd, Alpha);
			const FIntPoint Axial = Board->WorldToAxial(SamplePoint);

			if (AML_Tile* const* TilePtr = GridMap.Find(Axial))
			{
				AML_Tile* Tile = *TilePtr;
				if (IsValid(Tile) && Tile->IsBorderTile() && UML_HexPathfinder::IsTileWalkable(Tile))
					return Tile;
			}
		}
	}

	return nullptr;
}

AML_Tile* UML_NavigationBridgeComponent::FindReachableExitBorderTile(const AML_BoardSpawner* Board, const FVector& OutsideDestination) const
{
	if (!IsValid(Board) || !IsValid(PlayerCharacter) || !IsValid(PlayerCharacter->CurrentTileOn))
		return nullptr;

	const TMap<FIntPoint, AML_Tile*> GridMap = Board->GetGridMap();
	const FIntPoint StartAxial = PlayerCharacter->CurrentTileOn->GetAxialCoord();
	if (!GridMap.Contains(StartAxial))
		return nullptr;

	float MinDistSq = FLT_MAX;
	AML_Tile* ClosestReachableBorderTile = nullptr;

	for (const TPair<FIntPoint, AML_Tile*>& Pair : GridMap)
	{
		AML_Tile* Tile = Pair.Value;
		if (!IsValid(Tile) || !Tile->IsBorderTile() || !UML_HexPathfinder::IsTileWalkable(Tile))
			continue;

		TArray<FIntPoint> BoardPath;
		if (!UML_HexPathfinder::BuildPath_AxialBFS(StartAxial, Pair.Key, GridMap, BoardPath))
			continue;

		UNavigationPath* NavPath = UNavigationSystemV1::FindPathToLocationSynchronously(
			GetWorld(),
			Tile->GetActorLocation(),
			OutsideDestination);

		if (!IsCompleteNavMeshPath(NavPath, OutsideDestination))
			continue;

		const float DistSq = FVector::DistSquared(Tile->GetActorLocation(), OutsideDestination);
		if (DistSq < MinDistSq)
		{
			MinDistSq = DistSq;
			ClosestReachableBorderTile = Tile;
		}
	}

	return ClosestReachableBorderTile;
}
