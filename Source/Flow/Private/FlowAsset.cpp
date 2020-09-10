#include "FlowAsset.h"
#include "FlowSettings.h"
#include "FlowSubsystem.h"

#include "Nodes/FlowNode.h"
#include "Nodes/Route/FlowNode_CustomEvent.h"
#include "Nodes/Route/FlowNode_Start.h"
#include "Nodes/Route/FlowNode_Finish.h"
#include "Nodes/Route/FlowNode_SubGraph.h"

UFlowAsset::UFlowAsset(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

#if WITH_EDITOR
void UFlowAsset::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	UFlowAsset* This = CastChecked<UFlowAsset>(InThis);
	Collector.AddReferencedObject(This->FlowGraph, This);

	Super::AddReferencedObjects(InThis, Collector);
}

TSharedPtr<IFlowGraphInterface> UFlowAsset::FlowGraphInterface = nullptr;

void UFlowAsset::SetFlowGraphInterface(TSharedPtr<IFlowGraphInterface> InFlowAssetEditor)
{
	check(!FlowGraphInterface.IsValid());
	FlowGraphInterface = InFlowAssetEditor;
}

UFlowNode* UFlowAsset::CreateNode(const UClass* NodeClass, UEdGraphNode* GraphNode)
{
	UFlowNode* NewNode = NewObject<UFlowNode>(this, NodeClass, NAME_None, RF_Transactional);
	NewNode->SetGraphNode(GraphNode);

	RegisterNode(GraphNode->NodeGuid, NewNode);
	return NewNode;
}

void UFlowAsset::RegisterNode(const FGuid& NewGuid, UFlowNode* NewNode)
{
	NewNode->SetGuid(NewGuid);
	Nodes.Emplace(NewGuid, NewNode);
}

void UFlowAsset::UnregisterNode(const FGuid& NodeGuid)
{
	Nodes.Remove(NodeGuid);
	Nodes.Compact();

	HarvestNodeConnections();
	MarkPackageDirty();
}
#endif

void UFlowAsset::HarvestNodeConnections()
{
	TMap<FName, FConnectedPin> Connections;

	// last moment to remove invalid nodes
	for (auto NodeIt = Nodes.CreateIterator(); NodeIt; ++NodeIt)
	{
		const TPair<FGuid, UFlowNode*>& Pair = *NodeIt;
		if (Pair.Value == nullptr)
		{
			NodeIt.RemoveCurrent();
		}
	}

	for (const TPair<FGuid, UFlowNode*>& Pair : Nodes)
	{
		UFlowNode* Node = Pair.Value;
		Connections.Empty();

		for (const UEdGraphPin* ThisPin : Node->GetGraphNode()->Pins)
		{
			if (ThisPin->Direction == EGPD_Output && ThisPin->LinkedTo.Num() > 0)
			{
				if (UEdGraphPin* LinkedPin = ThisPin->LinkedTo[0])
				{
					const UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
					Connections.Add(ThisPin->PinName, FConnectedPin(LinkedNode->NodeGuid, LinkedPin->PinName));
				}
			}
		}

#if WITH_EDITOR
		Node->SetFlags(RF_Transactional);
		Node->Modify();
#endif
		Node->SetConnections(Connections);

#if WITH_EDITOR
		Node->PostEditChange();
#endif
	}
}

UFlowNode* UFlowAsset::GetNode(const FGuid& Guid) const
{
	if (UFlowNode* Node = Nodes.FindRef(Guid))
	{
		return Node;
	}

	return nullptr;
}

void UFlowAsset::RecursiveFindNodesByClass(UFlowNode* Node, const TSubclassOf<UFlowNode> Class, uint8 Depth, TArray<UFlowNode*>& OutNodes) const
{
	if (Node)
	{
		// Record the node if it is the desired type
		if (Node->GetClass() == Class)
		{
			OutNodes.AddUnique(Node);
		}

		if (OutNodes.Num() == Depth)
		{
			return;
		}

		// Recurse
		for (UFlowNode* ConnectedNode : Node->GetConnectedNodes())
		{
			RecursiveFindNodesByClass(ConnectedNode, Class, Depth, OutNodes);
		}
	}
}

void UFlowAsset::AddInstance(UFlowAsset* NewInstance)
{
	ActiveInstances.Add(NewInstance);
}

int32 UFlowAsset::RemoveInstance(UFlowAsset* Instance)
{
#if WITH_EDITOR
	if (InspectedInstance.IsValid() && InspectedInstance.Get() == Instance)
	{
		SetInspectedInstance(NAME_None);
	}
#endif

	ActiveInstances.Remove(Instance);
	return ActiveInstances.Num();
}

void UFlowAsset::ClearInstances()
{
#if WITH_EDITOR
	if (InspectedInstance.IsValid())
	{
		SetInspectedInstance(NAME_None);
	}
#endif

	for (UFlowAsset* Instance : ActiveInstances)
	{
		for (UFlowNode* Node : Instance->ActiveNodes)
		{
			Node->Cleanup();
		}

		Instance->ActiveNodes.Empty();
	}

	ActiveInstances.Empty();
}

#if WITH_EDITOR
void UFlowAsset::GetInstanceDisplayNames(TArray<TSharedPtr<FName>>& OutDisplayNames) const
{
	for (UFlowAsset* Instance : ActiveInstances)
	{
		OutDisplayNames.Emplace(MakeShareable(new FName(Instance->GetDisplayName())));
	}
}

void UFlowAsset::SetInspectedInstance(const FName& NewInspectedInstanceName)
{
	if (NewInspectedInstanceName.IsNone())
	{
		InspectedInstance = nullptr;
	}
	else
	{
		for (UFlowAsset* ActiveInstance : ActiveInstances)
		{
			if (ActiveInstance && ActiveInstance->GetDisplayName() == NewInspectedInstanceName)
			{
				if (!InspectedInstance.IsValid() || InspectedInstance != ActiveInstance)
				{
					InspectedInstance = ActiveInstance;
				}
				break;
			}
		}
	}

	BroadcastRegenerateToolbars();
}
#endif

void UFlowAsset::InitInstance(UFlowAsset* InTemplateAsset)
{
	TemplateAsset = InTemplateAsset;

	for (TPair<FGuid, UFlowNode*>& Node : Nodes)
	{
		UFlowNode* NewInstance = NewObject<UFlowNode>(this, Node.Value->GetClass(), NAME_None, RF_Transient, Node.Value, false, nullptr);
		Node.Value = NewInstance;

		// there can be only one, automatically added while creating graph
		if (UFlowNode_Start* InNode = Cast<UFlowNode_Start>(NewInstance))
		{
			StartNode = InNode;
		}

		if (UFlowNode_CustomEvent* CustomEvent = Cast<UFlowNode_CustomEvent>(NewInstance))
		{
			const FName& EventName = CustomEvent->EventName;
			if (!EventName.IsNone() && !CustomEventNodes.Contains(CustomEvent->EventName))
			{
				CustomEventNodes.Emplace(CustomEvent->EventName, CustomEvent);
			}
		}
	}
}

void UFlowAsset::PreloadNodes()
{
	TArray<UFlowNode*> GraphEntryNodes = { StartNode };
	for (const TPair<FName, UFlowNode_CustomEvent*>& CustomEvent : CustomEventNodes)
	{
		GraphEntryNodes.Emplace(CustomEvent.Value);
	}

	// NOTE: this is just the example algorithm of gathering nodes for pre-load
	for (UFlowNode* EntryNode : GraphEntryNodes)
	{
		for (const TPair<TSubclassOf<UFlowNode>, int32>& Node : UFlowSettings::Get()->DefaultPreloadDepth)
		{
			if (Node.Value > 0)
			{
				TArray<UFlowNode*> FoundNodes;
				RecursiveFindNodesByClass(EntryNode, Node.Key, Node.Value, FoundNodes);

				for (UFlowNode* FoundNode : FoundNodes)
				{
					if (!PreloadedNodes.Contains(FoundNode))
					{
						FoundNode->TriggerPreload();
						PreloadedNodes.Emplace(FoundNode);
					}
				}
			}
		}
	}
}

void UFlowAsset::FlushPreload()
{
	for (UFlowNode* PreloadedNode : PreloadedNodes)
	{
		PreloadedNode->TriggerFlush();
	}
	PreloadedNodes.Empty();
}

void UFlowAsset::StartFlow()
{
	ResetNodes();

#if WITH_EDITOR
	if (TemplateAsset->ActiveInstances.Num() == 1)
	{
		TemplateAsset->SetInspectedInstance(GetDisplayName());
	}
	else
	{
		TemplateAsset->BroadcastRegenerateToolbars();
	}
#endif
	
	ensureAlways(StartNode);
	RecordedNodes.Add(StartNode);
	StartNode->TriggerFirstOutput(true);
}

void UFlowAsset::StartSubFlow(UFlowNode_SubGraph* SubGraphNode)
{
	NodeOwningThisAssetInstance = SubGraphNode;
	NodeOwningThisAssetInstance->GetFlowAsset()->ActiveSubGraphs.Add(SubGraphNode, this);

	StartFlow();
}

TWeakObjectPtr<UFlowAsset> UFlowAsset::GetFlowInstance(UFlowNode_SubGraph* SubGraphNode) const
{
	return ActiveSubGraphs.FindRef(SubGraphNode);
}

void UFlowAsset::TriggerCustomEvent(UFlowNode_SubGraph* Node, const FName& EventName)
{
	const TWeakObjectPtr<UFlowAsset> FlowInstance = ActiveSubGraphs.FindRef(Node);
	if (FlowInstance.IsValid())
	{
		if (UFlowNode_CustomEvent* CustomEvent = FlowInstance->CustomEventNodes.FindRef(EventName))
		{
			RecordedNodes.Add(CustomEvent);
			CustomEvent->TriggerFirstOutput(true);
		}
	}
}

void UFlowAsset::TriggerCustomOutput(const FName& EventName) const
{
	NodeOwningThisAssetInstance->TriggerOutput(EventName);
}

void UFlowAsset::TriggerInput(const FGuid& NodeGuid, const FName& PinName)
{
	if (UFlowNode* Node = Nodes.FindRef(NodeGuid))
	{
		if (!ActiveNodes.Contains(Node))
		{
			ActiveNodes.Add(Node);
			RecordedNodes.Add(Node);
		}

		Node->TriggerInput(PinName);
	}
}

void UFlowAsset::FinishNode(UFlowNode* Node)
{
	if (ActiveNodes.Contains(Node))
	{
		ActiveNodes.Remove(Node);

		if (Node->GetClass()->IsChildOf(UFlowNode_Finish::StaticClass()) && NodeOwningThisAssetInstance.IsValid())
		{
			NodeOwningThisAssetInstance.Get()->TriggerFirstOutput(true);
			NodeOwningThisAssetInstance = nullptr;
		}
	}
}

void UFlowAsset::ResetNodes()
{
#if !UE_BUILD_SHIPPING
	for (UFlowNode* Node : RecordedNodes)
	{
		Node->ResetRecords();
	}
#endif

	RecordedNodes.Empty();
}

UFlowSubsystem* UFlowAsset::GetFlowSubsystem() const
{
	return Cast<UFlowSubsystem>(GetOuter());
}

FName UFlowAsset::GetDisplayName() const
{
	return GetFName();
}

UFlowNode_SubGraph* UFlowAsset::GetNodeOwningThisAssetInstance() const
{
	return NodeOwningThisAssetInstance.Get();
}

UFlowAsset* UFlowAsset::GetMasterInstance() const
{
	return NodeOwningThisAssetInstance.IsValid() ? NodeOwningThisAssetInstance.Get()->GetFlowAsset() : nullptr;
}

UFlowNode* UFlowAsset::GetNodeInstance(const FGuid Guid) const
{
	return Nodes.FindRef(Guid);
}