// example/main.go - 资源管理器使用示例
package main

import (
	"fmt"
	"log"

	"github.com/example/gores/example/generated"
)

func main() {
	// 示例1: 使用生成的资源管理器
	demonstrateGeneratedManager()

	// 示例2: 手动创建测试数据并使用
	demonstrateWithTestData()
}

func demonstrateGeneratedManager() {
	fmt.Println("=== 资源管理器使用示例 ===")
	fmt.Println()

	// 获取物品配置管理器单例
	itemMgr := generated.GetItemConfigManager()

	// 加载资源文件 (实际使用时)
	// err := itemMgr.Load("./data/item_config.bin")
	// if err != nil {
	//     log.Fatalf("加载物品配置失败: %v", err)
	// }

	fmt.Println("ItemConfigManager 已就绪")
	fmt.Printf("当前物品数量: %d\n", itemMgr.Count())
	fmt.Println()

	// 获取技能配置管理器
	skillMgr := generated.GetSkillConfigManager()
	fmt.Println("SkillConfigManager 已就绪")
	fmt.Printf("当前技能数量: %d\n", skillMgr.Count())
	fmt.Println()

	// 获取怪物配置管理器
	monsterMgr := generated.GetMonsterConfigManager()
	fmt.Println("MonsterConfigManager 已就绪")
	fmt.Printf("当前怪物数量: %d\n", monsterMgr.Count())
}

func demonstrateWithTestData() {
	fmt.Println()
	fmt.Println("=== 使用测试数据演示 ===")
	fmt.Println()

	// 创建一些测试物品
	items := []*generated.ItemConfig{
		{
			Id:            1001,
			Name:          "铁剑",
			Description:   "一把普通的铁剑",
			Code:          "iron_sword",
			Type:          generated.ItemType_TYPE_WEAPON,
			Quality:       generated.ItemQuality_QUALITY_COMMON,
			LevelRequired: 1,
			Price:         100,
			StackLimit:    1,
			Tradeable:     true,
			Icon:          "icons/weapons/iron_sword.png",
			Tags:          []string{"weapon", "sword", "iron"},
		},
		{
			Id:            1002,
			Name:          "钢剑",
			Description:   "一把锋利的钢剑",
			Code:          "steel_sword",
			Type:          generated.ItemType_TYPE_WEAPON,
			Quality:       generated.ItemQuality_QUALITY_UNCOMMON,
			LevelRequired: 10,
			Price:         500,
			StackLimit:    1,
			Tradeable:     true,
			Icon:          "icons/weapons/steel_sword.png",
			Tags:          []string{"weapon", "sword", "steel"},
		},
		{
			Id:            1003,
			Name:          "生命药水",
			Description:   "恢复100点生命值",
			Code:          "hp_potion",
			Type:          generated.ItemType_TYPE_CONSUMABLE,
			Quality:       generated.ItemQuality_QUALITY_COMMON,
			LevelRequired: 1,
			Price:         50,
			StackLimit:    99,
			Tradeable:     true,
			Icon:          "icons/consumables/hp_potion.png",
			Tags:          []string{"consumable", "potion", "healing"},
		},
		{
			Id:            1004,
			Name:          "铁矿石",
			Description:   "用于锻造的铁矿石",
			Code:          "iron_ore",
			Type:          generated.ItemType_TYPE_MATERIAL,
			Quality:       generated.ItemQuality_QUALITY_COMMON,
			LevelRequired: 1,
			Price:         10,
			StackLimit:    999,
			Tradeable:     true,
			Icon:          "icons/materials/iron_ore.png",
			Tags:          []string{"material", "ore", "iron"},
		},
		{
			Id:            1005,
			Name:          "传说之剑",
			Description:   "蕴含神秘力量的传说武器",
			Code:          "legendary_sword",
			Type:          generated.ItemType_TYPE_WEAPON,
			Quality:       generated.ItemQuality_QUALITY_LEGENDARY,
			LevelRequired: 50,
			Price:         100000,
			StackLimit:    1,
			Tradeable:     false,
			Icon:          "icons/weapons/legendary_sword.png",
			Tags:          []string{"weapon", "sword", "legendary"},
			Attributes: []*generated.ItemAttribute{
				{Name: "attack", Value: 500, Rate: 1.5},
				{Name: "critical", Value: 20, Rate: 1.0},
			},
		},
	}

	// 手动模拟加载数据 (实际中由Load方法完成)
	fmt.Println("模拟加载物品数据...")
	fmt.Printf("加载了 %d 个物品\n", len(items))
	fmt.Println()

	// 演示各种访问方式
	fmt.Println("--- 主键访问 ---")
	for _, item := range items {
		fmt.Printf("ID: %d, 名称: %s, 类型: %v, 品质: %v\n",
			item.Id, item.Name, item.Type, item.Quality)
	}
	fmt.Println()

	fmt.Println("--- 按类型分组 ---")
	typeGroups := make(map[generated.ItemType][]*generated.ItemConfig)
	for _, item := range items {
		typeGroups[item.Type] = append(typeGroups[item.Type], item)
	}
	for itemType, group := range typeGroups {
		fmt.Printf("类型 %v: %d 个物品\n", itemType, len(group))
		for _, item := range group {
			fmt.Printf("  - %s\n", item.Name)
		}
	}
	fmt.Println()

	fmt.Println("--- 按品质分组 ---")
	qualityGroups := make(map[generated.ItemQuality][]*generated.ItemConfig)
	for _, item := range items {
		qualityGroups[item.Quality] = append(qualityGroups[item.Quality], item)
	}
	for quality, group := range qualityGroups {
		fmt.Printf("品质 %v: %d 个物品\n", quality, len(group))
		for _, item := range group {
			fmt.Printf("  - %s (价格: %d)\n", item.Name, item.Price)
		}
	}
	fmt.Println()

	fmt.Println("--- 唯一索引查找 (按code) ---")
	codes := []string{"iron_sword", "hp_potion", "legendary_sword"}
	for _, code := range codes {
		for _, item := range items {
			if item.Code == code {
				fmt.Printf("Code '%s' -> %s\n", code, item.Name)
				break
			}
		}
	}
	fmt.Println()

	fmt.Println("--- 过滤查询 ---")
	fmt.Println("可交易的物品:")
	for _, item := range items {
		if item.Tradeable {
			fmt.Printf("  - %s (价格: %d)\n", item.Name, item.Price)
		}
	}
	fmt.Println()

	fmt.Println("等级要求 >= 10 的物品:")
	for _, item := range items {
		if item.LevelRequired >= 10 {
			fmt.Printf("  - %s (需要等级: %d)\n", item.Name, item.LevelRequired)
		}
	}
	fmt.Println()

	fmt.Println("--- 嵌套消息访问 ---")
	for _, item := range items {
		if len(item.Attributes) > 0 {
			fmt.Printf("%s 的属性:\n", item.Name)
			for _, attr := range item.Attributes {
				fmt.Printf("  - %s: %d (系数: %.2f)\n", attr.Name, attr.Value, attr.Rate)
			}
		}
	}
	fmt.Println()

	log.Println("演示完成!")
}
