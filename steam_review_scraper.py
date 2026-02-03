#!/usr/bin/env python3
"""
Steam Review Scraper - 异步并发爬取Steam游戏评论

功能特性:
- 异步并发请求,提高爬取效率
- 支持断点续传,避免重复爬取
- 自动保存评论到CSV文件
- 完整的错误处理和重试机制

使用方法:
    python steam_review_scraper.py [--app-id APP_ID] [--concurrency N] [--output FILE]
"""

import asyncio
import aiohttp
import aiofiles
import csv
import json
import os
import time
import argparse
import logging
from datetime import datetime, timedelta
from typing import Optional, Set, Dict, Any, List
from dataclasses import dataclass, asdict
from pathlib import Path
from tqdm.asyncio import tqdm

# 配置日志
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler('scraper.log', encoding='utf-8'),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)


@dataclass
class Review:
    """评论数据结构"""
    recommendationid: str           # 评论唯一ID
    steamid: str                    # 用户Steam ID
    author_playtime_forever: int    # 总游戏时长(分钟)
    author_playtime_at_review: int  # 评论时游戏时长(分钟)
    voted_up: bool                  # 是否推荐
    votes_up: int                   # 点赞数
    votes_funny: int                # 有趣数
    comment_count: int              # 评论数
    timestamp_created: int          # 评论创建时间戳
    timestamp_updated: int          # 评论更新时间戳
    review: str                     # 评论内容
    language: str                   # 语言
    weighted_vote_score: float      # 加权评分
    received_for_free: bool         # 是否免费获得游戏
    written_during_early_access: bool  # 是否早期测试期间评论
    
    def to_dict(self) -> Dict[str, Any]:
        """转换为字典,处理时间戳"""
        data = asdict(self)
        # 转换时间戳为可读格式
        data['created_time'] = datetime.fromtimestamp(
            self.timestamp_created
        ).strftime('%Y-%m-%d %H:%M:%S')
        data['updated_time'] = datetime.fromtimestamp(
            self.timestamp_updated
        ).strftime('%Y-%m-%d %H:%M:%S')
        return data


@dataclass
class CheckpointData:
    """断点续传数据"""
    cursor: str
    fetched_ids: Set[str]
    total_fetched: int
    last_update: str
    
    def to_dict(self) -> Dict[str, Any]:
        return {
            'cursor': self.cursor,
            'fetched_ids': list(self.fetched_ids),
            'total_fetched': self.total_fetched,
            'last_update': self.last_update
        }
    
    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> 'CheckpointData':
        return cls(
            cursor=data.get('cursor', '*'),
            fetched_ids=set(data.get('fetched_ids', [])),
            total_fetched=data.get('total_fetched', 0),
            last_update=data.get('last_update', '')
        )


class SteamReviewScraper:
    """Steam评论爬虫"""
    
    # Steam API基础URL
    BASE_URL = "https://store.steampowered.com/appreviews/{app_id}"
    
    # CSV文件字段
    CSV_FIELDS = [
        'recommendationid', 'steamid', 'author_playtime_forever',
        'author_playtime_at_review', 'voted_up', 'votes_up', 'votes_funny',
        'comment_count', 'timestamp_created', 'timestamp_updated',
        'created_time', 'updated_time', 'review', 'language',
        'weighted_vote_score', 'received_for_free', 'written_during_early_access'
    ]
    
    def __init__(
        self,
        app_id: int = 4128260,
        output_file: str = 'steam_reviews.csv',
        checkpoint_file: str = 'checkpoint.json',
        concurrency: int = 5,
        day_range: int = 30,
        retry_times: int = 3,
        retry_delay: float = 2.0
    ):
        """
        初始化爬虫
        
        Args:
            app_id: Steam游戏ID
            output_file: 输出CSV文件路径
            checkpoint_file: 断点文件路径
            concurrency: 并发请求数
            day_range: 拉取天数范围
            retry_times: 重试次数
            retry_delay: 重试延迟(秒)
        """
        self.app_id = app_id
        self.output_file = output_file
        self.checkpoint_file = checkpoint_file
        self.concurrency = concurrency
        self.day_range = day_range
        self.retry_times = retry_times
        self.retry_delay = retry_delay
        
        # 状态追踪
        self.fetched_ids: Set[str] = set()
        self.cursor = '*'
        self.total_fetched = 0
        self.session: Optional[aiohttp.ClientSession] = None
        self.semaphore: Optional[asyncio.Semaphore] = None
        
        # 请求队列
        self.request_queue: asyncio.Queue = asyncio.Queue()
        self.result_queue: asyncio.Queue = asyncio.Queue()
        
        # 统计信息
        self.stats = {
            'total_requests': 0,
            'successful_requests': 0,
            'failed_requests': 0,
            'duplicate_reviews': 0,
            'new_reviews': 0
        }
    
    def _build_params(self, cursor: str = '*') -> Dict[str, Any]:
        """
        构建API请求参数
        
        Args:
            cursor: 分页游标
            
        Returns:
            请求参数字典
        """
        return {
            'json': '1',                    # 返回JSON格式
            'filter': 'recent',             # 最近的评论
            'language': 'all',              # 所有语言
            'day_range': str(self.day_range),  # 天数范围
            'cursor': cursor,               # 分页游标
            'review_type': 'all',           # 所有类型评论
            'purchase_type': 'all',         # 所有购买类型
            'num_per_page': '100',          # 每页数量(最大100)
        }
    
    async def _load_checkpoint(self) -> None:
        """加载断点数据"""
        if os.path.exists(self.checkpoint_file):
            try:
                async with aiofiles.open(self.checkpoint_file, 'r', encoding='utf-8') as f:
                    content = await f.read()
                    data = json.loads(content)
                    checkpoint = CheckpointData.from_dict(data)
                    self.cursor = checkpoint.cursor
                    self.fetched_ids = checkpoint.fetched_ids
                    self.total_fetched = checkpoint.total_fetched
                    logger.info(
                        f"已加载断点: cursor={self.cursor[:20]}..., "
                        f"已获取 {self.total_fetched} 条评论"
                    )
            except Exception as e:
                logger.warning(f"加载断点失败: {e}, 将从头开始")
    
    async def _save_checkpoint(self) -> None:
        """保存断点数据"""
        checkpoint = CheckpointData(
            cursor=self.cursor,
            fetched_ids=self.fetched_ids,
            total_fetched=self.total_fetched,
            last_update=datetime.now().isoformat()
        )
        try:
            async with aiofiles.open(self.checkpoint_file, 'w', encoding='utf-8') as f:
                await f.write(json.dumps(checkpoint.to_dict(), ensure_ascii=False))
            logger.debug("断点已保存")
        except Exception as e:
            logger.error(f"保存断点失败: {e}")
    
    async def _load_existing_ids(self) -> None:
        """从现有CSV文件加载已爬取的评论ID"""
        if os.path.exists(self.output_file):
            try:
                async with aiofiles.open(self.output_file, 'r', encoding='utf-8') as f:
                    content = await f.read()
                    reader = csv.DictReader(content.splitlines())
                    for row in reader:
                        if 'recommendationid' in row:
                            self.fetched_ids.add(row['recommendationid'])
                logger.info(f"已从CSV加载 {len(self.fetched_ids)} 条已有评论ID")
            except Exception as e:
                logger.warning(f"加载已有评论ID失败: {e}")
    
    async def _init_csv(self) -> None:
        """初始化CSV文件(如果不存在则创建表头)"""
        if not os.path.exists(self.output_file):
            async with aiofiles.open(self.output_file, 'w', encoding='utf-8', newline='') as f:
                writer_content = ','.join(self.CSV_FIELDS) + '\n'
                await f.write(writer_content)
            logger.info(f"已创建CSV文件: {self.output_file}")
    
    async def _append_to_csv(self, reviews: List[Review]) -> None:
        """
        追加评论到CSV文件
        
        Args:
            reviews: 评论列表
        """
        if not reviews:
            return
            
        async with aiofiles.open(self.output_file, 'a', encoding='utf-8', newline='') as f:
            for review in reviews:
                data = review.to_dict()
                # 处理评论内容中的特殊字符
                row_values = []
                for field in self.CSV_FIELDS:
                    value = data.get(field, '')
                    if isinstance(value, str):
                        # 转义双引号并用双引号包围
                        value = '"' + value.replace('"', '""').replace('\n', ' ').replace('\r', '') + '"'
                    else:
                        value = str(value)
                    row_values.append(value)
                await f.write(','.join(row_values) + '\n')
    
    async def _fetch_reviews(self, cursor: str) -> Optional[Dict[str, Any]]:
        """
        发送单个API请求获取评论
        
        Args:
            cursor: 分页游标
            
        Returns:
            API响应数据
        """
        url = self.BASE_URL.format(app_id=self.app_id)
        params = self._build_params(cursor)
        
        for attempt in range(self.retry_times):
            try:
                async with self.semaphore:
                    async with self.session.get(
                        url,
                        params=params,
                        timeout=aiohttp.ClientTimeout(total=30)
                    ) as response:
                        self.stats['total_requests'] += 1
                        
                        if response.status == 200:
                            data = await response.json()
                            if data.get('success') == 1:
                                self.stats['successful_requests'] += 1
                                return data
                            else:
                                logger.warning(f"API返回失败: {data}")
                        else:
                            logger.warning(f"HTTP错误: {response.status}")
                            
            except asyncio.TimeoutError:
                logger.warning(f"请求超时 (尝试 {attempt + 1}/{self.retry_times})")
            except aiohttp.ClientError as e:
                logger.warning(f"请求错误: {e} (尝试 {attempt + 1}/{self.retry_times})")
            except Exception as e:
                logger.error(f"未知错误: {e} (尝试 {attempt + 1}/{self.retry_times})")
            
            if attempt < self.retry_times - 1:
                await asyncio.sleep(self.retry_delay * (attempt + 1))
        
        self.stats['failed_requests'] += 1
        return None
    
    def _parse_review(self, review_data: Dict[str, Any]) -> Optional[Review]:
        """
        解析单条评论数据
        
        Args:
            review_data: 原始评论数据
            
        Returns:
            Review对象
        """
        try:
            author = review_data.get('author', {})
            return Review(
                recommendationid=str(review_data.get('recommendationid', '')),
                steamid=str(author.get('steamid', '')),
                author_playtime_forever=author.get('playtime_forever', 0),
                author_playtime_at_review=author.get('playtime_at_review', 0),
                voted_up=review_data.get('voted_up', False),
                votes_up=review_data.get('votes_up', 0),
                votes_funny=review_data.get('votes_funny', 0),
                comment_count=review_data.get('comment_count', 0),
                timestamp_created=review_data.get('timestamp_created', 0),
                timestamp_updated=review_data.get('timestamp_updated', 0),
                review=review_data.get('review', ''),
                language=review_data.get('language', ''),
                weighted_vote_score=float(review_data.get('weighted_vote_score', 0)),
                received_for_free=review_data.get('received_for_free', False),
                written_during_early_access=review_data.get('written_during_early_access', False)
            )
        except Exception as e:
            logger.error(f"解析评论失败: {e}")
            return None
    
    async def _process_response(self, data: Dict[str, Any]) -> tuple[List[Review], str, int]:
        """
        处理API响应
        
        Args:
            data: API响应数据
            
        Returns:
            (新评论列表, 下一个cursor, 总评论数)
        """
        reviews_data = data.get('reviews', [])
        query_summary = data.get('query_summary', {})
        next_cursor = data.get('cursor', '')
        total_reviews = query_summary.get('total_reviews', 0)
        
        new_reviews = []
        for review_data in reviews_data:
            review = self._parse_review(review_data)
            if review:
                if review.recommendationid not in self.fetched_ids:
                    self.fetched_ids.add(review.recommendationid)
                    new_reviews.append(review)
                    self.stats['new_reviews'] += 1
                else:
                    self.stats['duplicate_reviews'] += 1
        
        return new_reviews, next_cursor, total_reviews
    
    async def run(self) -> None:
        """运行爬虫主流程"""
        logger.info(f"开始爬取Steam游戏评论 - App ID: {self.app_id}")
        logger.info(f"参数: 并发数={self.concurrency}, 天数范围={self.day_range}天")
        
        # 初始化
        await self._load_checkpoint()
        await self._load_existing_ids()
        await self._init_csv()
        
        self.semaphore = asyncio.Semaphore(self.concurrency)
        
        connector = aiohttp.TCPConnector(
            limit=self.concurrency * 2,
            ttl_dns_cache=300
        )
        
        async with aiohttp.ClientSession(
            connector=connector,
            headers={
                'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36',
                'Accept': 'application/json',
                'Accept-Language': 'en-US,en;q=0.9,zh-CN;q=0.8,zh;q=0.7'
            }
        ) as session:
            self.session = session
            
            current_cursor = self.cursor
            total_reviews = None
            batch_count = 0
            
            # 创建进度条
            pbar = tqdm(desc="爬取评论", unit="条", dynamic_ncols=True)
            
            try:
                while True:
                    # 保存当前cursor用于比较
                    old_cursor = current_cursor
                    
                    # 发送请求获取评论
                    data = await self._fetch_reviews(current_cursor)
                    
                    if data is None:
                        logger.error("获取评论失败,停止爬取")
                        break
                    
                    new_reviews, next_cursor, total = await self._process_response(data)
                    
                    if total_reviews is None:
                        total_reviews = total
                        pbar.total = total_reviews
                        logger.info(f"总评论数: {total_reviews}")
                    
                    # 保存新评论
                    if new_reviews:
                        await self._append_to_csv(new_reviews)
                        self.total_fetched += len(new_reviews)
                        pbar.update(len(new_reviews))
                    
                    # 定期保存断点
                    batch_count += 1
                    if batch_count % 5 == 0:
                        await self._save_checkpoint()
                    
                    # 检查是否还有更多评论
                    num_reviews = data.get('query_summary', {}).get('num_reviews', 0)
                    
                    # 退出条件:
                    # 1. 没有返回评论
                    # 2. 没有下一个cursor
                    # 3. cursor没有变化(已到达末尾)
                    if num_reviews == 0 or not next_cursor or next_cursor == old_cursor:
                        logger.info("已获取所有评论")
                        break
                    
                    # 更新cursor
                    current_cursor = next_cursor
                    self.cursor = current_cursor
                    
                    # 添加小延迟避免请求过快
                    await asyncio.sleep(0.3)
                    
            except KeyboardInterrupt:
                logger.info("用户中断,保存进度...")
            finally:
                pbar.close()
                await self._save_checkpoint()
        
        # 打印统计信息
        self._print_stats()
    
    def _print_stats(self) -> None:
        """打印统计信息"""
        logger.info("=" * 50)
        logger.info("爬取统计:")
        logger.info(f"  总请求数: {self.stats['total_requests']}")
        logger.info(f"  成功请求: {self.stats['successful_requests']}")
        logger.info(f"  失败请求: {self.stats['failed_requests']}")
        logger.info(f"  新评论数: {self.stats['new_reviews']}")
        logger.info(f"  重复评论: {self.stats['duplicate_reviews']}")
        logger.info(f"  总获取数: {self.total_fetched}")
        logger.info(f"  输出文件: {self.output_file}")
        logger.info("=" * 50)


def parse_args():
    """解析命令行参数"""
    parser = argparse.ArgumentParser(
        description='Steam游戏评论爬虫 - 异步并发爬取',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
    # 使用默认参数爬取
    python steam_review_scraper.py
    
    # 指定游戏ID和并发数
    python steam_review_scraper.py --app-id 4128260 --concurrency 10
    
    # 指定输出文件
    python steam_review_scraper.py --output my_reviews.csv
        """
    )
    
    parser.add_argument(
        '--app-id',
        type=int,
        default=4128260,
        help='Steam游戏ID (默认: 4128260)'
    )
    
    parser.add_argument(
        '--output', '-o',
        type=str,
        default='steam_reviews.csv',
        help='输出CSV文件路径 (默认: steam_reviews.csv)'
    )
    
    parser.add_argument(
        '--checkpoint', '-c',
        type=str,
        default='checkpoint.json',
        help='断点文件路径 (默认: checkpoint.json)'
    )
    
    parser.add_argument(
        '--concurrency', '-n',
        type=int,
        default=5,
        help='并发请求数 (默认: 5)'
    )
    
    parser.add_argument(
        '--day-range', '-d',
        type=int,
        default=30,
        help='拉取天数范围 (默认: 30)'
    )
    
    parser.add_argument(
        '--retry',
        type=int,
        default=3,
        help='重试次数 (默认: 3)'
    )
    
    return parser.parse_args()


async def main():
    """主函数"""
    args = parse_args()
    
    scraper = SteamReviewScraper(
        app_id=args.app_id,
        output_file=args.output,
        checkpoint_file=args.checkpoint,
        concurrency=args.concurrency,
        day_range=args.day_range,
        retry_times=args.retry
    )
    
    await scraper.run()


if __name__ == '__main__':
    asyncio.run(main())
