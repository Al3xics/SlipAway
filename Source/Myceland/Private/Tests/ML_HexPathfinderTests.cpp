// Copyright Myceland Team, All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/ML_CoreData.h"
#include "Developer Settings/ML_MycelandDeveloperSettings.h"
#include "Player/ML_HexPathfinder.h"
#include "Tiles/ML_BoardSpawner.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Pathfinding rules of the tile-by-tile movement, pinned without a world.
 * UML_HexPathfinder::FindPath_AStar takes the step rule as a callback, so these tests feed it a small hex
 * grid described by a height map (a cell missing from the map does not exist), and the real height rule
 * from a transient settings object — never the project's config, so the tests do not depend on it.
 */
namespace MLHexPathfinderTests
{
	// Hexagon of the given radius around (0,0), every cell at level 0.
	TMap<FIntPoint, int32> MakeFlatHexagon(const int32 Radius)
	{
		TMap<FIntPoint, int32> Heights;
		for (int32 Q = -Radius; Q <= Radius; ++Q)
			for (int32 R = FMath::Max(-Radius, -Q - Radius); R <= FMath::Min(Radius, -Q + Radius); ++R)
				Heights.Add(FIntPoint(Q, R), 0);
		return Heights;
	}

	bool FindPath(const TMap<FIntPoint, int32>& Heights, const UML_MycelandDeveloperSettings* Settings,
	              const FIntPoint& Start, const FIntPoint& Goal, TArray<FIntPoint>& OutPath,
	              const bool bAllowPartial = false, const int32 MaxExplored = 4096)
	{
		return UML_HexPathfinder::FindPath_AStar(Start, Goal,
			[&Heights, Settings](const FIntPoint& From, const FIntPoint& To)
			{
				const int32* FromLevel = Heights.Find(From);
				const int32* ToLevel = Heights.Find(To);
				return FromLevel && ToLevel && Settings->IsHeightStepAllowed(*FromLevel, *ToLevel);
			},
			MaxExplored, bAllowPartial, OutPath);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FML_HexHeightRuleTest,
                                 "Myceland.Movement.HeightRule",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FML_HexHeightRuleTest::RunTest(const FString& Parameters)
{
	UML_MycelandDeveloperSettings* Settings = NewObject<UML_MycelandDeveloperSettings>();
	Settings->MaxClimbLevels = 1;

	Settings->bCanDropAnyHeight = false;
	TestTrue(TEXT("Flat step"), Settings->IsHeightStepAllowed(0, 0));
	TestTrue(TEXT("Climb one level"), Settings->IsHeightStepAllowed(0, 1));
	TestFalse(TEXT("Climb two levels"), Settings->IsHeightStepAllowed(0, 2));
	TestTrue(TEXT("Drop one level"), Settings->IsHeightStepAllowed(1, 0));
	TestFalse(TEXT("Drop two levels without Can Drop Any Height"), Settings->IsHeightStepAllowed(2, 0));

	Settings->bCanDropAnyHeight = true;
	TestTrue(TEXT("Drop two levels with Can Drop Any Height"), Settings->IsHeightStepAllowed(2, 0));
	TestFalse(TEXT("Can Drop Any Height never allows a bigger climb"), Settings->IsHeightStepAllowed(0, 2));

	Settings->MaxClimbLevels = 2;
	TestTrue(TEXT("Max Climb Levels 2 climbs two levels"), Settings->IsHeightStepAllowed(0, 2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FML_HexAStarTest,
                                 "Myceland.Movement.AStar",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FML_HexAStarTest::RunTest(const FString& Parameters)
{
	using namespace MLHexPathfinderTests;

	UML_MycelandDeveloperSettings* Settings = NewObject<UML_MycelandDeveloperSettings>();
	Settings->MaxClimbLevels = 1;
	Settings->bCanDropAnyHeight = false;

	const FIntPoint Origin(0, 0);
	const FIntPoint Goal(2, 0);
	const FIntPoint Middle(1, 0);
	TArray<FIntPoint> Path;

	TestEqual(TEXT("Hex distance to a neighbor"), UML_HexPathfinder::HexDistance(Origin, FIntPoint(1, -1)), 1);
	TestEqual(TEXT("Hex distance across the board"), UML_HexPathfinder::HexDistance(FIntPoint(-2, 0), FIntPoint(2, 0)), 4);

	// ---------- Flat ground ----------
	TMap<FIntPoint, int32> Heights = MakeFlatHexagon(3);
	TestTrue(TEXT("Straight line is found"), FindPath(Heights, Settings, Origin, Goal, Path));
	TestEqual(TEXT("Straight line is the shortest (3 cells, both ends included)"), Path.Num(), 3);
	TestEqual(TEXT("Path starts on Start"), Path.Num() > 0 ? Path[0] : FIntPoint(-99, -99), Origin);
	TestEqual(TEXT("Path ends on Goal"), Path.Num() > 0 ? Path.Last() : FIntPoint(-99, -99), Goal);

	for (int32 Index = 1; Index < Path.Num(); ++Index)
		TestEqual(TEXT("Every step goes to a neighbor"), UML_HexPathfinder::HexDistance(Path[Index - 1], Path[Index]), 1);

	TestTrue(TEXT("Start == Goal is a one-cell path"), FindPath(Heights, Settings, Origin, Origin, Path));
	TestEqual(TEXT("Start == Goal path length"), Path.Num(), 1);

	// ---------- Climbing ----------
	Heights[Middle] = 1;
	TestTrue(TEXT("One level up is walked straight over"), FindPath(Heights, Settings, Origin, Goal, Path));
	TestEqual(TEXT("One level up does not cost a detour"), Path.Num(), 3);

	Heights[Middle] = 2;
	TestTrue(TEXT("Two levels up is walked around"), FindPath(Heights, Settings, Origin, Goal, Path));
	TestFalse(TEXT("The detour never steps on the two-level cell"), Path.Contains(Middle));
	TestEqual(TEXT("The detour is one cell longer"), Path.Num(), 4);

	// ---------- Dropping ----------
	Heights = MakeFlatHexagon(3);
	Heights[Origin] = 2; // Start on a pillar, everything around two levels lower.

	TestFalse(TEXT("No way down from a pillar without Can Drop Any Height"), FindPath(Heights, Settings, Origin, Goal, Path));
	TestEqual(TEXT("A failed search without partial leaves the path empty"), Path.Num(), 0);

	Settings->bCanDropAnyHeight = true;
	TestTrue(TEXT("Can Drop Any Height jumps off the pillar"), FindPath(Heights, Settings, Origin, Goal, Path));
	Settings->bCanDropAnyHeight = false;

	// ---------- Unreachable goal, partial path ----------
	Heights = MakeFlatHexagon(3);
	for (const FIntPoint& Dir : Directions)
		Heights[Goal + Dir] = 5; // A two-level wall all around Goal.

	const FIntPoint FarStart(-3, 0); // 5 steps from Goal, so the partial path has somewhere to go.
	TestFalse(TEXT("A walled-in goal is unreachable"), FindPath(Heights, Settings, FarStart, Goal, Path));
	TestFalse(TEXT("Partial search still reports the goal as unreached"), FindPath(Heights, Settings, FarStart, Goal, Path, /*bAllowPartial=*/true));
	TestTrue(TEXT("Partial path moves toward the goal"), Path.Num() > 1);
	if (Path.Num() > 0)
	{
		TestEqual(TEXT("Partial path starts on Start"), Path[0], FarStart);
		TestEqual(TEXT("Partial path ends as close to the goal as the wall allows"), UML_HexPathfinder::HexDistance(Path.Last(), Goal), 2);
		TestFalse(TEXT("Partial path never climbs the wall"), Path.ContainsByPredicate([&Heights](const FIntPoint& Cell) { return Heights[Cell] > 1; }));
	}

	// ---------- Search cap ----------
	Heights = MakeFlatHexagon(10);
	TestFalse(TEXT("A goal beyond the explored-cells cap is given up on"),
	          FindPath(Heights, Settings, FIntPoint(-10, 0), FIntPoint(10, 0), Path, false, /*MaxExplored=*/5));
	TestTrue(TEXT("The same goal is found without the cap"),
	         FindPath(Heights, Settings, FIntPoint(-10, 0), FIntPoint(10, 0), Path));

	return true;
}

/**
 * The "stop on the first tile of a puzzle" rule. Boards are stand-in keys (addresses of local ints): the rule only
 * compares board identities and asks whether a board stops the player, so no actor is needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FML_HexBoardEntryCutTest,
                                 "Myceland.Movement.BoardEntryCut",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FML_HexBoardEntryCutTest::RunTest(const FString& Parameters)
{
	int32 WorldA = 0, WorldB = 0, OpenPuzzle = 0, OtherPuzzle = 0, SolvedPuzzle = 0;
	const void* W1 = &WorldA;
	const void* W2 = &WorldB;
	const void* P  = &OpenPuzzle;
	const void* P2 = &OtherPuzzle;
	const void* S  = &SolvedPuzzle; // a solved / locked puzzle: walked across like terrain

	const auto Stops = [P, P2](const void* Board) { return Board == P || Board == P2; };
	const auto Cut = [&Stops](const TArray<const void*>& Steps)
	{
		return UML_HexPathfinder::ComputeBoardEntryCut(Steps, Stops);
	};

	TestEqual(TEXT("Empty path"), Cut({}), 0);
	TestEqual(TEXT("Walking the world is never cut"), Cut({W1, W1, W1, W1}), 4);
	TestEqual(TEXT("Crossing world boards is never cut"), Cut({W1, W1, W2, W2}), 4);
	TestEqual(TEXT("Walking into a puzzle stops on its first tile"), Cut({W1, W1, P, P, P}), 3);
	TestEqual(TEXT("A path ending on the puzzle's first tile is kept whole"), Cut({W1, W1, P}), 3);
	TestEqual(TEXT("A move inside the puzzle the player stands in is never cut"), Cut({P, P, P, P}), 4);
	TestEqual(TEXT("Walking out of a puzzle is never cut"), Cut({P, P, W1, W1}), 4);
	TestEqual(TEXT("Leaving a puzzle and coming back stops on re-entry"), Cut({P, W1, W1, P, P}), 4);
	TestEqual(TEXT("Going straight from one puzzle into another stops on the second"), Cut({P, P, P2, P2}), 3);
	TestEqual(TEXT("A solved puzzle is crossed like terrain"), Cut({W1, S, S, W1}), 4);
	TestEqual(TEXT("Crossing a solved puzzle then entering an open one stops there"), Cut({W1, S, S, P, P}), 4);
	TestEqual(TEXT("A step on no board never stops the player"), Cut({W1, nullptr, W1}), 3);
	return true;
}

/**
 * Global lattice math. Runs on the project's own Hex Grid settings (size / orientation / origin), so it checks
 * properties that hold whatever those values are.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FML_HexLatticeTest,
                                 "Myceland.Movement.Lattice",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FML_HexLatticeTest::RunTest(const FString& Parameters)
{
	const UML_MycelandDeveloperSettings* Settings = UML_MycelandDeveloperSettings::GetMycelandDeveloperSettings();
	const float TileSize = FMath::Max(Settings->HexTileSize, 1.f);

	for (int32 Q = -6; Q <= 6; ++Q)
	{
		for (int32 R = -6; R <= 6; ++R)
		{
			const FIntPoint Axial(Q, R);
			const FVector2D Center = AML_BoardSpawner::AxialToLocal2D(Q, R);

			if (AML_BoardSpawner::Local2DToAxial(Center) != Axial)
				AddError(FString::Printf(TEXT("Cell %d,%d: its center does not map back to it"), Q, R));

			// Anywhere well inside the hexagon (inner radius = size * sqrt(3)/2) maps to the same cell.
			const float Inside = TileSize * 0.8f;
			for (const FVector2D& Offset : {FVector2D(Inside, 0.f), FVector2D(-Inside, 0.f), FVector2D(0.f, Inside), FVector2D(0.f, -Inside)})
			{
				if (AML_BoardSpawner::Local2DToAxial(Center + Offset * 0.75f) != Axial)
					AddError(FString::Printf(TEXT("Cell %d,%d: a point inside it maps to another cell"), Q, R));
			}

			// Neighbors are exactly one tile-width apart (2 * inner radius = size * sqrt(3)).
			for (const FIntPoint& Dir : Directions)
			{
				const FVector2D NeighborCenter = AML_BoardSpawner::AxialToLocal2D(Q + Dir.X, R + Dir.Y);
				const double Expected = static_cast<double>(TileSize) * 1.7320508;
				if (!FMath::IsNearlyEqual(FVector2D::Distance(Center, NeighborCenter), Expected, 0.1))
					AddError(FString::Printf(TEXT("Cell %d,%d: a neighbor is not one tile-width away"), Q, R));
			}
		}
	}

	// Snapping lands on a lattice point, never moves farther than one tile, and is stable.
	for (const FVector& Point : {FVector(37.f, -12.f, 5.f), FVector(1234.5f, 987.f, -40.f), FVector(-310.f, 72.f, 0.f)})
	{
		const FVector Snapped = AML_BoardSpawner::SnapToLattice(Point);
		TestEqual(TEXT("Snapping keeps Z"), Snapped.Z, Point.Z);
		TestTrue(TEXT("Snapping moves less than one tile"), FVector::Dist2D(Point, Snapped) <= TileSize + 0.1f);
		TestTrue(TEXT("Snapping a snapped point does nothing"), FVector::Dist2D(Snapped, AML_BoardSpawner::SnapToLattice(Snapped)) < 0.01f);
	}

	return true;
}

/** Height levels and the climb height given to the character, on a transient settings object. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FML_HexHeightLevelTest,
                                 "Myceland.Movement.HeightLevels",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FML_HexHeightLevelTest::RunTest(const FString& Parameters)
{
	UML_MycelandDeveloperSettings* Settings = NewObject<UML_MycelandDeveloperSettings>();
	Settings->HexGridOrigin = FVector(0.f, 0.f, 100.f);
	Settings->TileHeightStep = 50.f;
	Settings->MaxClimbLevels = 1;

	TestEqual(TEXT("Origin Z is level 0"), Settings->GetHeightLevelFromZ(100.f), 0);
	TestEqual(TEXT("One step up is level 1"), Settings->GetHeightLevelFromZ(150.f), 1);
	TestEqual(TEXT("Below the origin is negative"), Settings->GetHeightLevelFromZ(50.f), -1);
	TestEqual(TEXT("Slightly off a level rounds to it"), Settings->GetHeightLevelFromZ(158.f), 1);
	TestEqual(TEXT("Past half a step rounds up"), Settings->GetHeightLevelFromZ(176.f), 2);
	TestEqual(TEXT("Level to Z"), Settings->GetZFromHeightLevel(2), 200.f);

	for (int32 Level = -3; Level <= 5; ++Level)
		TestEqual(TEXT("Z of a level maps back to it"), Settings->GetHeightLevelFromZ(Settings->GetZFromHeightLevel(Level)), Level);

	TestEqual(TEXT("Climb height = one level"), Settings->GetMaxClimbHeight(), 50.f);
	Settings->MaxClimbLevels = 2;
	TestEqual(TEXT("Climb height = two levels"), Settings->GetMaxClimbHeight(), 100.f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
