// Copyright Myceland Team, All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ML_TileBase.generated.h"

UCLASS()
class MYCELAND_API AML_TileBase : public AActor
{
	GENERATED_BODY()

public:
	AML_TileBase();
	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable,
	Category="Myceland Tile|Feedback",
	meta=(DisplayName="Set Tile Alive"))
	void SetTileAlive();

protected:
	virtual void BeginPlay() override;
	
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Myceland Tile", meta=(AllowPrivateAccess = "true"))
	USceneComponent* SceneRoot;
	
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Myceland Tile", meta=(AllowPrivateAccess = "true"))
	UStaticMeshComponent* GroundBase;

};
